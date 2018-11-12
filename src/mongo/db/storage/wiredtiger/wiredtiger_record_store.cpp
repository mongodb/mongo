// wiredtiger_record_store.cpp


/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage
#define LOG_FOR_RECOVERY(level) \
    MONGO_LOG_COMPONENT(level, ::mongo::logger::LogComponent::kStorageRecovery)

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/static_assert.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_stones.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::unique_ptr;
using std::string;

namespace {

static const int kMinimumRecordStoreVersion = 1;
static const int kCurrentRecordStoreVersion = 1;  // New record stores use this by default.
static const int kMaximumRecordStoreVersion = 1;
MONGO_STATIC_ASSERT(kCurrentRecordStoreVersion >= kMinimumRecordStoreVersion);
MONGO_STATIC_ASSERT(kCurrentRecordStoreVersion <= kMaximumRecordStoreVersion);

void checkOplogFormatVersion(OperationContext* opCtx, const std::string& uri) {
    StatusWith<BSONObj> appMetadata = WiredTigerUtil::getApplicationMetadata(opCtx, uri);
    fassert(39999, appMetadata);

    fassertNoTrace(39998, appMetadata.getValue().getIntField("oplogKeyExtractionVersion") == 1);
}
}  // namespace

MONGO_FAIL_POINT_DEFINE(WTWriteConflictException);
MONGO_FAIL_POINT_DEFINE(WTWriteConflictExceptionForReads);

const std::string kWiredTigerEngineName = "wiredTiger";

// For a capped collection, the number of documents that can be removed directly, rather than via a
// truncate.  The value has been determined somewhat by experimentation, but there's no clear win
// for all situations.  Setting it to a lower number makes individual remove calls happen, rather
// than truncate, only when small numbers of documents are inserted at a time. Making it larger
// makes larger chunks of documents inserted at time follow the remove path in preference to the
// truncate path.  Using direct removes is more likely to be a benefit when inserts are spread over
// many capped collections, since avoiding a truncate avoids having to get a second cursor, which
// may not be already cached in the current session. The benefit becomes less pronounced if the
// capped collections are more actively used, or are used in small number of sessions, as multiple
// cursors will be available in the needed session caches.
static int kCappedDocumentRemoveLimit = 3;

class WiredTigerRecordStore::OplogStones::InsertChange final : public RecoveryUnit::Change {
public:
    InsertChange(OplogStones* oplogStones,
                 int64_t bytesInserted,
                 RecordId highestInserted,
                 int64_t countInserted)
        : _oplogStones(oplogStones),
          _bytesInserted(bytesInserted),
          _highestInserted(highestInserted),
          _countInserted(countInserted) {}

    void commit(boost::optional<Timestamp>) final {
        invariant(_bytesInserted >= 0);
        invariant(_highestInserted.isValid());

        _oplogStones->_currentRecords.addAndFetch(_countInserted);
        int64_t newCurrentBytes = _oplogStones->_currentBytes.addAndFetch(_bytesInserted);
        if (newCurrentBytes >= _oplogStones->_minBytesPerStone) {
            _oplogStones->createNewStoneIfNeeded(_highestInserted);
        }
    }

    void rollback() final {}

private:
    OplogStones* _oplogStones;
    int64_t _bytesInserted;
    RecordId _highestInserted;
    int64_t _countInserted;
};

class WiredTigerRecordStore::OplogStones::TruncateChange final : public RecoveryUnit::Change {
public:
    TruncateChange(OplogStones* oplogStones) : _oplogStones(oplogStones) {}

    void commit(boost::optional<Timestamp>) final {
        _oplogStones->_currentRecords.store(0);
        _oplogStones->_currentBytes.store(0);

        stdx::lock_guard<stdx::mutex> lk(_oplogStones->_mutex);
        _oplogStones->_stones.clear();
    }

    void rollback() final {}

private:
    OplogStones* _oplogStones;
};

WiredTigerRecordStore::OplogStones::OplogStones(OperationContext* opCtx, WiredTigerRecordStore* rs)
    : _rs(rs) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    invariant(rs->isCapped());
    invariant(rs->cappedMaxSize() > 0);
    unsigned long long maxSize = rs->cappedMaxSize();

    const unsigned long long kMinStonesToKeep = 10ULL;
    const unsigned long long kMaxStonesToKeep = 100ULL;

    unsigned long long numStones = maxSize / BSONObjMaxInternalSize;
    size_t numStonesToKeep = std::min(kMaxStonesToKeep, std::max(kMinStonesToKeep, numStones));
    _minBytesPerStone = maxSize / numStonesToKeep;
    invariant(_minBytesPerStone > 0);

    _calculateStones(opCtx, numStonesToKeep);
    _pokeReclaimThreadIfNeeded();  // Reclaim stones if over the limit.
}

bool WiredTigerRecordStore::OplogStones::isDead() {
    stdx::lock_guard<stdx::mutex> lk(_oplogReclaimMutex);
    return _isDead;
}

void WiredTigerRecordStore::OplogStones::kill() {
    {
        stdx::lock_guard<stdx::mutex> lk(_oplogReclaimMutex);
        _isDead = true;
    }
    _oplogReclaimCv.notify_one();
}

void WiredTigerRecordStore::OplogStones::awaitHasExcessStonesOrDead() {
    // Wait until kill() is called or there are too many oplog stones.
    stdx::unique_lock<stdx::mutex> lock(_oplogReclaimMutex);
    while (!_isDead) {
        {
            MONGO_IDLE_THREAD_BLOCK;
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (hasExcessStones_inlock()) {
                // There are now excess oplog stones. However, there it may be necessary to keep
                // additional oplog.
                //
                // During startup or after rollback, the current state of the data goes "back in
                // time" and replication recovery replays oplog entries to bring the data to a
                // desired state. This process may require more oplog than the user dictated oplog
                // size allotment.
                auto stone = _stones.front();
                invariant(stone.lastRecord.isValid());
                if (static_cast<std::uint64_t>(stone.lastRecord.repr()) <
                    _rs->getPinnedOplog().asULL()) {
                    break;
                }
            }
        }
        _oplogReclaimCv.wait(lock);
    }
}

boost::optional<WiredTigerRecordStore::OplogStones::Stone>
WiredTigerRecordStore::OplogStones::peekOldestStoneIfNeeded() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (!hasExcessStones_inlock()) {
        return {};
    }

    return _stones.front();
}

void WiredTigerRecordStore::OplogStones::popOldestStone() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _stones.pop_front();
}

void WiredTigerRecordStore::OplogStones::createNewStoneIfNeeded(RecordId lastRecord) {
    stdx::unique_lock<stdx::mutex> lk(_mutex, stdx::try_to_lock);
    if (!lk) {
        // Someone else is either already creating a new stone or popping the oldest one. In the
        // latter case, we let the next insert trigger the new stone's creation.
        return;
    }

    if (_currentBytes.load() < _minBytesPerStone) {
        // Must have raced to create a new stone, someone else already triggered it.
        return;
    }

    if (!_stones.empty() && lastRecord < _stones.back().lastRecord) {
        // Skip creating a new stone when the record's position comes before the most recently
        // created stone. We likely raced with another batch of inserts that caused us to try and
        // make multiples stones.
        return;
    }

    LOG(2) << "create new oplogStone, current stones:" << _stones.size();
    OplogStones::Stone stone = {_currentRecords.swap(0), _currentBytes.swap(0), lastRecord};
    _stones.push_back(stone);

    _pokeReclaimThreadIfNeeded();
}

void WiredTigerRecordStore::OplogStones::updateCurrentStoneAfterInsertOnCommit(
    OperationContext* opCtx,
    int64_t bytesInserted,
    RecordId highestInserted,
    int64_t countInserted) {
    opCtx->recoveryUnit()->registerChange(
        new InsertChange(this, bytesInserted, highestInserted, countInserted));
}

void WiredTigerRecordStore::OplogStones::clearStonesOnCommit(OperationContext* opCtx) {
    opCtx->recoveryUnit()->registerChange(new TruncateChange(this));
}

void WiredTigerRecordStore::OplogStones::updateStonesAfterCappedTruncateAfter(
    int64_t recordsRemoved, int64_t bytesRemoved, RecordId firstRemovedId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    int64_t numStonesToRemove = 0;
    int64_t recordsInStonesToRemove = 0;
    int64_t bytesInStonesToRemove = 0;

    // Compute the number and associated sizes of the records from stones that are either fully or
    // partially truncated.
    for (auto it = _stones.rbegin(); it != _stones.rend(); ++it) {
        if (it->lastRecord < firstRemovedId) {
            break;
        }
        numStonesToRemove++;
        recordsInStonesToRemove += it->records;
        bytesInStonesToRemove += it->bytes;
    }

    // Remove the stones corresponding to the records that were deleted.
    int64_t offset = _stones.size() - numStonesToRemove;
    _stones.erase(_stones.begin() + offset, _stones.end());

    // Account for any remaining records from a partially truncated stone in the stone currently
    // being filled.
    _currentRecords.addAndFetch(recordsInStonesToRemove - recordsRemoved);
    _currentBytes.addAndFetch(bytesInStonesToRemove - bytesRemoved);
}

void WiredTigerRecordStore::OplogStones::setMinBytesPerStone(int64_t size) {
    invariant(size > 0);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Only allow changing the minimum bytes per stone if no data has been inserted.
    invariant(_stones.size() == 0 && _currentRecords.load() == 0);
    _minBytesPerStone = size;
}

void WiredTigerRecordStore::OplogStones::_calculateStones(OperationContext* opCtx,
                                                          size_t numStonesToKeep) {
    long long numRecords = _rs->numRecords(opCtx);
    long long dataSize = _rs->dataSize(opCtx);

    log() << "The size storer reports that the oplog contains " << numRecords
          << " records totaling to " << dataSize << " bytes";

    // Only use sampling to estimate where to place the oplog stones if the number of samples drawn
    // is less than 5% of the collection.
    const uint64_t kMinSampleRatioForRandCursor = 20;

    // If the oplog doesn't contain enough records to make sampling more efficient, then scan the
    // oplog to determine where to put down stones.
    if (numRecords <= 0 || dataSize <= 0 ||
        uint64_t(numRecords) <
            kMinSampleRatioForRandCursor * kRandomSamplesPerStone * numStonesToKeep) {
        _calculateStonesByScanning(opCtx);
        return;
    }

    // Use the oplog's average record size to estimate the number of records in each stone, and thus
    // estimate the combined size of the records.
    double avgRecordSize = double(dataSize) / double(numRecords);
    double estRecordsPerStone = std::ceil(_minBytesPerStone / avgRecordSize);
    double estBytesPerStone = estRecordsPerStone * avgRecordSize;

    _calculateStonesBySampling(opCtx, int64_t(estRecordsPerStone), int64_t(estBytesPerStone));
}

void WiredTigerRecordStore::OplogStones::_calculateStonesByScanning(OperationContext* opCtx) {
    log() << "Scanning the oplog to determine where to place markers for truncation";

    long long numRecords = 0;
    long long dataSize = 0;

    auto cursor = _rs->getCursor(opCtx, true);
    while (auto record = cursor->next()) {
        _currentRecords.addAndFetch(1);
        int64_t newCurrentBytes = _currentBytes.addAndFetch(record->data.size());
        if (newCurrentBytes >= _minBytesPerStone) {
            LOG(1) << "Placing a marker at optime "
                   << Timestamp(record->id.repr()).toStringPretty();

            OplogStones::Stone stone = {_currentRecords.swap(0), _currentBytes.swap(0), record->id};
            _stones.push_back(stone);
        }

        numRecords++;
        dataSize += record->data.size();
    }

    _rs->updateStatsAfterRepair(opCtx, numRecords, dataSize);
}

void WiredTigerRecordStore::OplogStones::_calculateStonesBySampling(OperationContext* opCtx,
                                                                    int64_t estRecordsPerStone,
                                                                    int64_t estBytesPerStone) {
    Timestamp earliestOpTime;
    Timestamp latestOpTime;

    {
        const bool forward = true;
        auto cursor = _rs->getCursor(opCtx, forward);
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the oplog just in case.
            log() << "Failed to determine the earliest optime, falling back to scanning the oplog";
            _calculateStonesByScanning(opCtx);
            return;
        }
        earliestOpTime = Timestamp(record->id.repr());
    }

    {
        const bool forward = false;
        auto cursor = _rs->getCursor(opCtx, forward);
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the oplog just in case.
            log() << "Failed to determine the latest optime, falling back to scanning the oplog";
            _calculateStonesByScanning(opCtx);
            return;
        }
        latestOpTime = Timestamp(record->id.repr());
    }

    log() << "Sampling from the oplog between " << earliestOpTime.toStringPretty() << " and "
          << latestOpTime.toStringPretty() << " to determine where to place markers for truncation";

    int64_t wholeStones = _rs->numRecords(opCtx) / estRecordsPerStone;
    int64_t numSamples = kRandomSamplesPerStone * _rs->numRecords(opCtx) / estRecordsPerStone;

    log() << "Taking " << numSamples << " samples and assuming that each section of oplog contains"
          << " approximately " << estRecordsPerStone << " records totaling to " << estBytesPerStone
          << " bytes";

    // Inform the random cursor of the number of samples we intend to take. This allows it to
    // account for skew in the tree shape.
    const std::string extraConfig = str::stream() << "next_random_sample_size=" << numSamples;

    // Divide the oplog into 'wholeStones' logical sections, with each section containing
    // approximately 'estRecordsPerStone'. Do so by oversampling the oplog, sorting the samples in
    // order of their RecordId, and then choosing the samples expected to be near the right edge of
    // each logical section.
    auto cursor = _rs->getRandomCursorWithOptions(opCtx, extraConfig);
    std::vector<RecordId> oplogEstimates;
    for (int i = 0; i < numSamples; ++i) {
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the oplog just in case.
            log() << "Failed to get enough random samples, falling back to scanning the oplog";
            _calculateStonesByScanning(opCtx);
            return;
        }
        oplogEstimates.push_back(record->id);
    }
    std::sort(oplogEstimates.begin(), oplogEstimates.end());

    for (int i = 1; i <= wholeStones; ++i) {
        // Use every (kRandomSamplesPerStone)th sample, starting with the
        // (kRandomSamplesPerStone - 1)th, as the last record for each stone.
        int sampleIndex = kRandomSamplesPerStone * i - 1;
        RecordId lastRecord = oplogEstimates[sampleIndex];

        log() << "Placing a marker at optime " << Timestamp(lastRecord.repr()).toStringPretty();
        OplogStones::Stone stone = {estRecordsPerStone, estBytesPerStone, lastRecord};
        _stones.push_back(stone);
    }

    // Account for the partially filled chunk.
    _currentRecords.store(_rs->numRecords(opCtx) - estRecordsPerStone * wholeStones);
    _currentBytes.store(_rs->dataSize(opCtx) - estBytesPerStone * wholeStones);
}

void WiredTigerRecordStore::OplogStones::_pokeReclaimThreadIfNeeded() {
    if (hasExcessStones_inlock()) {
        _oplogReclaimCv.notify_one();
    }
}

void WiredTigerRecordStore::OplogStones::adjust(int64_t maxSize) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    const unsigned long long kMinStonesToKeep = 10ULL;
    const unsigned long long kMaxStonesToKeep = 100ULL;

    unsigned long long numStones = maxSize / BSONObjMaxInternalSize;
    size_t numStonesToKeep = std::min(kMaxStonesToKeep, std::max(kMinStonesToKeep, numStones));
    _minBytesPerStone = maxSize / numStonesToKeep;
    invariant(_minBytesPerStone > 0);
    _pokeReclaimThreadIfNeeded();
}

StatusWith<std::string> WiredTigerRecordStore::parseOptionsField(const BSONObj options) {
    StringBuilder ss;
    BSONForEach(elem, options) {
        if (elem.fieldNameStringData() == "configString") {
            Status status = WiredTigerUtil::checkTableCreationOptions(elem);
            if (!status.isOK()) {
                return status;
            }
            ss << elem.valueStringData() << ',';
        } else {
            // Return error on first unrecognized field.
            return StatusWith<std::string>(ErrorCodes::InvalidOptions,
                                           str::stream() << '\'' << elem.fieldNameStringData()
                                                         << '\''
                                                         << " is not a supported option.");
        }
    }
    return StatusWith<std::string>(ss.str());
}

class WiredTigerRecordStore::RandomCursor final : public RecordCursor {
public:
    RandomCursor(OperationContext* opCtx, const WiredTigerRecordStore& rs, StringData config)
        : _cursor(nullptr), _rs(&rs), _opCtx(opCtx), _config(config.toString() + ",next_random") {
        restore();
    }

    ~RandomCursor() {
        if (_cursor)
            detachFromOperationContext();
    }

    boost::optional<Record> next() final {
        int advanceRet =
            wiredTigerPrepareConflictRetry(_opCtx, [&] { return _cursor->next(_cursor); });
        if (advanceRet == WT_NOTFOUND)
            return {};
        invariantWTOK(advanceRet);

        int64_t key;
        invariantWTOK(_cursor->get_key(_cursor, &key));
        const RecordId id = RecordId(key);

        WT_ITEM value;
        invariantWTOK(_cursor->get_value(_cursor, &value));

        return {{id, {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
    }

    void save() final {
        if (_cursor && !wt_keeptxnopen()) {
            try {
                _cursor->reset(_cursor);
            } catch (const WriteConflictException&) {
                // Ignore since this is only called when we are about to kill our transaction
                // anyway.
            }
        }
    }

    bool restore() final {
        // We can't use the CursorCache since this cursor needs a special config string.
        WT_SESSION* session = WiredTigerRecoveryUnit::get(_opCtx)->getSession()->getSession();

        if (!_cursor) {
            invariantWTOK(session->open_cursor(
                session, _rs->_uri.c_str(), nullptr, _config.c_str(), &_cursor));
            invariant(_cursor);
        }
        return true;
    }

    void detachFromOperationContext() final {
        invariant(_opCtx);
        _opCtx = nullptr;
        if (_cursor) {
            invariantWTOK(_cursor->close(_cursor));
        }
        _cursor = nullptr;
    }

    void reattachToOperationContext(OperationContext* opCtx) final {
        invariant(!_opCtx);
        _opCtx = opCtx;
    }

private:
    WT_CURSOR* _cursor;
    const WiredTigerRecordStore* _rs;
    OperationContext* _opCtx;
    const std::string _config;
};


// static
StatusWith<std::string> WiredTigerRecordStore::generateCreateString(
    const std::string& engineName,
    StringData ns,
    const CollectionOptions& options,
    StringData extraStrings,
    const bool prefixed) {
    // Separate out a prefix and suffix in the default string. User configuration will
    // override values in the prefix, but not values in the suffix.
    str::stream ss;
    ss << "type=file,";
    // Setting this larger than 10m can hurt latencies and throughput degradation if this
    // is the oplog.  See SERVER-16247
    ss << "memory_page_max=10m,";
    // Choose a higher split percent, since most usage is append only. Allow some space
    // for workloads where updates increase the size of documents.
    ss << "split_pct=90,";
    ss << "leaf_value_max=64MB,";
    ss << "checksum=on,";
    if (wiredTigerGlobalOptions.useCollectionPrefixCompression) {
        ss << "prefix_compression,";
    }

    ss << "block_compressor=" << wiredTigerGlobalOptions.collectionBlockCompressor << ",";

    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())->getTableCreateConfig(ns);

    ss << extraStrings << ",";

    StatusWith<std::string> customOptions =
        parseOptionsField(options.storageEngine.getObjectField(engineName));
    if (!customOptions.isOK())
        return customOptions;

    ss << customOptions.getValue();

    if (NamespaceString::oplog(ns)) {
        // force file for oplog
        ss << "type=file,";
        // Tune down to 10m.  See SERVER-16247
        ss << "memory_page_max=10m,";
    }

    // WARNING: No user-specified config can appear below this line. These options are required
    // for correct behavior of the server.
    if (prefixed) {
        ss << "key_format=qq";
    } else {
        ss << "key_format=q";
    }
    ss << ",value_format=u";

    // Record store metadata
    ss << ",app_metadata=(formatVersion=" << kCurrentRecordStoreVersion;
    if (NamespaceString::oplog(ns)) {
        ss << ",oplogKeyExtractionVersion=1";
    }
    ss << ")";

    bool replicatedWrites = getGlobalReplSettings().usingReplSets() ||
        repl::ReplSettings::shouldRecoverFromOplogAsStandalone();

    // Do not journal writes when 'ns' is an empty string, which is the case for internal-only
    // temporary tables.
    if (ns.size() && WiredTigerUtil::useTableLogging(NamespaceString(ns), replicatedWrites)) {
        ss << ",log=(enabled=true)";
    } else {
        ss << ",log=(enabled=false)";
    }

    return StatusWith<std::string>(ss);
}

WiredTigerRecordStore::WiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                             OperationContext* ctx,
                                             Params params)
    : RecordStore(params.ns),
      _uri(WiredTigerKVEngine::kTableUriPrefix + params.ident),
      _ident(params.ident),
      _tableId(WiredTigerSession::genTableId()),
      _engineName(params.engineName),
      _isCapped(params.isCapped),
      _isEphemeral(params.isEphemeral),
      _isOplog(NamespaceString::oplog(params.ns)),
      _cappedMaxSize(params.cappedMaxSize),
      _cappedMaxSizeSlack(std::min(params.cappedMaxSize / 10, int64_t(16 * 1024 * 1024))),
      _cappedMaxDocs(params.cappedMaxDocs),
      _cappedSleep(0),
      _cappedSleepMS(0),
      _cappedCallback(params.cappedCallback),
      _shuttingDown(false),
      _cappedDeleteCheckCount(0),
      _sizeStorer(params.sizeStorer),
      _kvEngine(kvEngine) {
    invariant(_ident.size() > 0);

    Status versionStatus = WiredTigerUtil::checkApplicationMetadataFormatVersion(
                               ctx, _uri, kMinimumRecordStoreVersion, kMaximumRecordStoreVersion)
                               .getStatus();

    if (!versionStatus.isOK()) {
        std::cout << " Version: " << versionStatus.reason() << std::endl;
        if (versionStatus.code() == ErrorCodes::FailedToParse) {
            uasserted(28548, versionStatus.reason());
        } else {
            fassertFailedNoTrace(34433);
        }
    }

    if (_isCapped) {
        invariant(_cappedMaxSize > 0);
        invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
    } else {
        invariant(_cappedMaxSize == -1);
        invariant(_cappedMaxDocs == -1);
    }

    if (!params.isReadOnly && !isTemp()) {
        bool replicatedWrites = getGlobalReplSettings().usingReplSets() ||
            repl::ReplSettings::shouldRecoverFromOplogAsStandalone();
        uassertStatusOK(WiredTigerUtil::setTableLogging(
            ctx, _uri, WiredTigerUtil::useTableLogging(NamespaceString(ns()), replicatedWrites)));
    }

    if (_isOplog) {
        checkOplogFormatVersion(ctx, _uri);
        // The oplog always needs to be marked for size adjustment since it is journaled and also
        // may change during replication recovery (if truncated).
        sizeRecoveryState(getGlobalServiceContext())
            .markCollectionAsAlwaysNeedsSizeAdjustment(_ident);
    }
}

WiredTigerRecordStore::~WiredTigerRecordStore() {
    {
        stdx::lock_guard<stdx::mutex> lk(_cappedCallbackMutex);
        _shuttingDown = true;
    }

    if (!isTemp()) {
        LOG(1) << "~WiredTigerRecordStore for: " << ns();
    } else {
        LOG(1) << "~WiredTigerRecordStore for temporary ident: " << getIdent();
    }

    if (_oplogStones) {
        _oplogStones->kill();
    }

    if (_isOplog) {
        // Delete oplog visibility manager on KV engine.
        _kvEngine->haltOplogManager();
    }
}

void WiredTigerRecordStore::postConstructorInit(OperationContext* opCtx) {
    // Find the largest RecordId currently in use and estimate the number of records.
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, /*forward=*/false);
    _sizeInfo =
        _sizeStorer ? _sizeStorer->load(_uri) : std::make_shared<WiredTigerSizeStorer::SizeInfo>();

    if (auto record = cursor->next()) {
        int64_t max = record->id.repr();
        _nextIdNum.store(1 + max);

        if (!_sizeStorer) {
            LOG(1) << "Doing scan of collection " << ns() << " to get size and count info";

            int64_t numRecords = 0;
            int64_t dataSize = 0;
            do {
                numRecords++;
                dataSize += record->data.size();
            } while ((record = cursor->next()));
            _sizeInfo->numRecords.store(numRecords);
            _sizeInfo->dataSize.store(dataSize);
        }
    } else {
        // We found no records in this collection; however, there may actually be documents present
        // if writes to this collection were not included in the stable checkpoint the last time
        // this node shut down. We set the data size and the record count to zero, but will adjust
        // these if writes are played during startup recovery.
        // Alternatively, this may be a collection we are creating during replication recovery.
        // In that case the collection will be given a new ident and a new SizeStorer entry. The
        // collection size from before we recovered to stable timestamp is not associated with this
        // record store and so we must keep track of the count throughout recovery.
        //
        // We mark a RecordStore as needing size adjustment iff its size is accurate at the current
        // time but not as of the top of the oplog.
        LOG_FOR_RECOVERY(2) << "Record store was empty; setting count metadata to zero but marking "
                               "record store as needing size adjustment during recovery. ns: "
                            << (isTemp() ? "(temp)" : ns()) << ", ident: " << _ident;
        sizeRecoveryState(getGlobalServiceContext())
            .markCollectionAsAlwaysNeedsSizeAdjustment(_ident);
        _sizeInfo->dataSize.store(0);
        _sizeInfo->numRecords.store(0);

        // Need to start at 1 so we are always higher than RecordId::min()
        _nextIdNum.store(1);
    }

    if (_sizeStorer)
        _sizeStorer->store(_uri, _sizeInfo);

    if (WiredTigerKVEngine::initRsOplogBackgroundThread(ns())) {
        _oplogStones = std::make_shared<OplogStones>(opCtx, this);
    }

    if (_isOplog) {
        invariant(_kvEngine);
        _kvEngine->startOplogManager(opCtx, _uri, this);
    }
}

const char* WiredTigerRecordStore::name() const {
    return _engineName.c_str();
}

bool WiredTigerRecordStore::inShutdown() const {
    stdx::lock_guard<stdx::mutex> lk(_cappedCallbackMutex);
    return _shuttingDown;
}

long long WiredTigerRecordStore::dataSize(OperationContext* opCtx) const {
    return _sizeInfo->dataSize.load();
}

long long WiredTigerRecordStore::numRecords(OperationContext* opCtx) const {
    return _sizeInfo->numRecords.load();
}

bool WiredTigerRecordStore::isCapped() const {
    return _isCapped;
}

int64_t WiredTigerRecordStore::cappedMaxDocs() const {
    invariant(_isCapped);
    return _cappedMaxDocs;
}

int64_t WiredTigerRecordStore::cappedMaxSize() const {
    invariant(_isCapped);
    return _cappedMaxSize;
}

int64_t WiredTigerRecordStore::storageSize(OperationContext* opCtx,
                                           BSONObjBuilder* extraInfo,
                                           int infoLevel) const {
    dassert(opCtx->lockState()->isReadLocked());

    if (_isEphemeral) {
        return dataSize(opCtx);
    }
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();
    StatusWith<int64_t> result =
        WiredTigerUtil::getStatisticsValueAs<int64_t>(session->getSession(),
                                                      "statistics:" + getURI(),
                                                      "statistics=(size)",
                                                      WT_STAT_DSRC_BLOCK_SIZE);
    uassertStatusOK(result.getStatus());

    int64_t size = result.getValue();

    if (size == 0 && _isCapped) {
        // Many things assume an empty capped collection still takes up space.
        return 1;
    }
    return size;
}

// Retrieve the value from a positioned cursor.
RecordData WiredTigerRecordStore::_getData(const WiredTigerCursor& cursor) const {
    WT_ITEM value;
    invariantWTOK(cursor->get_value(cursor.get(), &value));

    return RecordData(static_cast<const char*>(value.data), value.size).getOwned();
}

bool WiredTigerRecordStore::findRecord(OperationContext* opCtx,
                                       const RecordId& id,
                                       RecordData* out) const {
    dassert(opCtx->lockState()->isReadLocked());

    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    setKey(c, id);
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
    if (ret == WT_NOTFOUND) {
        return false;
    }
    invariantWTOK(ret);
    *out = _getData(curwrap);
    return true;
}

void WiredTigerRecordStore::deleteRecord(OperationContext* opCtx, const RecordId& id) {
    dassert(opCtx->lockState()->isWriteLocked());

    // Deletes should never occur on a capped collection because truncation uses
    // WT_SESSION::truncate().
    invariant(!isCapped());

    WiredTigerCursor cursor(_uri, _tableId, true, opCtx);
    cursor.assertInActiveTxn();
    WT_CURSOR* c = cursor.get();
    setKey(c, id);
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
    invariantWTOK(ret);

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret);

    int64_t old_length = old_value.size;

    ret = WT_OP_CHECK(c->remove(c));
    invariantWTOK(ret);

    _changeNumRecords(opCtx, -1);
    _increaseDataSize(opCtx, -old_length);
}

bool WiredTigerRecordStore::cappedAndNeedDelete() const {
    if (!_isCapped)
        return false;

    if (_sizeInfo->dataSize.load() >= _cappedMaxSize)
        return true;

    if ((_cappedMaxDocs != -1) && (_sizeInfo->numRecords.load() > _cappedMaxDocs))
        return true;

    return false;
}

int64_t WiredTigerRecordStore::_cappedDeleteAsNeeded(OperationContext* opCtx,
                                                     const RecordId& justInserted) {
    // If the collection does not need size adjustment, then we are in replication recovery and
    // replaying operations we've already played. This may occur after rollback or after a shutdown.
    // Any inserts beyond the stable timestamp have been undone, but any documents deleted from
    // capped collections did not come back due to being performed in an untimestamped side
    // transaction. Additionally, the SizeStorer's information reflects the state of the collection
    // before rollback/shutdown, post capped deletions.
    //
    // If we have a RecordStore whose size we know accurately as of the stable timestamp, rather
    // than as of the top of the oplog, then we must actually perform capped deletions because they
    // have not previously been accounted for. The collection will be marked as needing size
    // adjustment when enterring this function.
    //
    // One edge case to consider is where we need to delete a document that we insert as part of
    // replication recovery. If we don't mark the collection for size adjustment then we will not
    // perform the capped deletions as expected. In that case, the collection is guaranteed to be
    // empty at the stable timestamp and thus guaranteed to be marked for size adjustment.
    if (!sizeRecoveryState(getGlobalServiceContext()).collectionNeedsSizeAdjustment(_ident)) {
        return 0;
    }

    invariant(!_oplogStones);

    // We only want to do the checks occasionally as they are expensive.
    // This variable isn't thread safe, but has loose semantics anyway.
    dassert(!_isOplog || _cappedMaxDocs == -1);

    if (!cappedAndNeedDelete())
        return 0;

    // ensure only one thread at a time can do deletes, otherwise they'll conflict.
    stdx::unique_lock<stdx::timed_mutex> lock(_cappedDeleterMutex, stdx::defer_lock);

    if (_cappedMaxDocs != -1) {
        lock.lock();  // Max docs has to be exact, so have to check every time.
    } else {
        if (!lock.try_lock()) {
            // Someone else is deleting old records. Apply back-pressure if too far behind,
            // otherwise continue.
            if ((_sizeInfo->dataSize.load() - _cappedMaxSize) < _cappedMaxSizeSlack)
                return 0;

            // Don't wait forever: we're in a transaction, we could block eviction.
            Date_t before = Date_t::now();
            bool gotLock = lock.try_lock_for(stdx::chrono::milliseconds(200));
            auto delay =
                stdx::chrono::milliseconds(durationCount<Milliseconds>(Date_t::now() - before));
            _cappedSleep.fetchAndAdd(1);
            _cappedSleepMS.fetchAndAdd(delay.count());
            if (!gotLock)
                return 0;

            // If we already waited, let someone else do cleanup unless we are significantly
            // over the limit.
            if ((_sizeInfo->dataSize.load() - _cappedMaxSize) < (2 * _cappedMaxSizeSlack))
                return 0;
        }
    }

    return _cappedDeleteAsNeeded_inlock(opCtx, justInserted);
}

Timestamp WiredTigerRecordStore::getPinnedOplog() const {
    return _kvEngine->getPinnedOplog();
}

void WiredTigerRecordStore::_positionAtFirstRecordId(OperationContext* opCtx,
                                                     WT_CURSOR* cursor,
                                                     const RecordId& firstRecordId,
                                                     bool forTruncate) const {
    // Use the previous first RecordId, if available, to navigate to the current first
    // RecordId. The straightforward algorithm of resetting the cursor and advancing to the first
    // element will be slow for capped collections since there may be many tombstones to traverse
    // at the beginning of the table.
    if (!firstRecordId.isNull()) {
        setKey(cursor, firstRecordId);
        // Truncate does not require its cursor to be explicitly positioned.
        if (!forTruncate) {
            int cmp = 0;
            int ret = wiredTigerPrepareConflictRetry(
                opCtx, [&] { return cursor->search_near(cursor, &cmp); });
            invariantWTOK(ret);

            // This is (or was) the first recordId, so it should never be the case that we have a
            // RecordId before that.
            invariant(cmp >= 0);
        }
    } else {
        invariantWTOK(WT_READ_CHECK(cursor->reset(cursor)));
        int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return cursor->next(cursor); });
        invariantWTOK(ret);
    }
}

int64_t WiredTigerRecordStore::_cappedDeleteAsNeeded_inlock(OperationContext* opCtx,
                                                            const RecordId& justInserted) {
    // we do this in a side transaction in case it aborts
    WiredTigerRecoveryUnit* realRecoveryUnit =
        checked_cast<WiredTigerRecoveryUnit*>(opCtx->releaseRecoveryUnit().release());
    invariant(realRecoveryUnit);
    WiredTigerSessionCache* sc = realRecoveryUnit->getSessionCache();
    WriteUnitOfWork::RecoveryUnitState const realRUstate =
        opCtx->setRecoveryUnit(std::make_unique<WiredTigerRecoveryUnit>(sc),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();

    int64_t dataSize = _sizeInfo->dataSize.load();
    int64_t numRecords = _sizeInfo->numRecords.load();

    int64_t sizeOverCap = (dataSize > _cappedMaxSize) ? dataSize - _cappedMaxSize : 0;
    int64_t sizeSaved = 0;
    int64_t docsOverCap = 0, docsRemoved = 0;
    if (_cappedMaxDocs != -1 && numRecords > _cappedMaxDocs)
        docsOverCap = numRecords - _cappedMaxDocs;

    try {
        WriteUnitOfWork wuow(opCtx);

        WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
        WT_CURSOR* truncateEnd = curwrap.get();
        RecordId newestIdToDelete;
        int ret = 0;
        bool positioned = false;  // Mark if the cursor is on the first key
        RecordId savedFirstKey;

        // If we know where the first record is, go to it
        if (_cappedFirstRecord != RecordId()) {
            setKey(truncateEnd, _cappedFirstRecord);
            ret = wiredTigerPrepareConflictRetry(opCtx,
                                                 [&] { return truncateEnd->search(truncateEnd); });
            if (ret == 0) {
                positioned = true;
                savedFirstKey = _cappedFirstRecord;
            }
        }

        // Advance the cursor truncateEnd until we find a suitable end point for our truncate
        while ((sizeSaved < sizeOverCap || docsRemoved < docsOverCap) && (docsRemoved < 20000) &&
               (positioned || (ret = wiredTigerPrepareConflictRetry(opCtx, [&] {
                                   return truncateEnd->next(truncateEnd);
                               })) == 0)) {
            positioned = false;

            newestIdToDelete = getKey(truncateEnd);
            // don't go past the record we just inserted
            if (newestIdToDelete >= justInserted)  // TODO: use oldest uncommitted instead
                break;

            WT_ITEM old_value;
            invariantWTOK(truncateEnd->get_value(truncateEnd, &old_value));

            ++docsRemoved;
            sizeSaved += old_value.size;

            stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
            if (_shuttingDown)
                break;

            if (_cappedCallback) {
                uassertStatusOK(_cappedCallback->aboutToDeleteCapped(
                    opCtx,
                    newestIdToDelete,
                    RecordData(static_cast<const char*>(old_value.data), old_value.size)));
            }
        }

        if (ret != WT_NOTFOUND) {
            invariantWTOK(ret);
        }

        if (docsRemoved > 0) {
            // if we scanned to the end of the collection or past our insert, go back one
            if (ret == WT_NOTFOUND || newestIdToDelete >= justInserted) {
                ret = wiredTigerPrepareConflictRetry(
                    opCtx, [&] { return truncateEnd->prev(truncateEnd); });
            }
            invariantWTOK(ret);

            RecordId firstRemainingId;
            ret = truncateEnd->next(truncateEnd);
            if (ret != WT_NOTFOUND) {
                invariantWTOK(ret);
                firstRemainingId = getKey(truncateEnd);
            }
            invariantWTOK(truncateEnd->prev(truncateEnd));  // put the cursor back where it was

            // Consider a direct removal, without the overhead of opening a second cursor, if we
            // are removing a small number of records.  In the oplog case, always use truncate
            // since we typically have a second oplog cursor cached.
            if (docsRemoved <= kCappedDocumentRemoveLimit) {
                RecordId firstRecordId = savedFirstKey;
                int toRemove = docsRemoved;

                // Remember the key that was removed between calls to remove, that saves time in
                // navigating to the next record.
                while (toRemove > 0) {
                    _positionAtFirstRecordId(opCtx, truncateEnd, firstRecordId, false);
                    if (--toRemove > 0) {
                        firstRecordId = getKey(truncateEnd);
                    }
                    invariantWTOK(truncateEnd->remove(truncateEnd));
                }
                ret = 0;
            } else {
                WiredTigerCursor startWrap(_uri, _tableId, true, opCtx);
                WT_CURSOR* truncateStart = startWrap.get();

                // Position the start cursor at the first record, even if we don't have a saved
                // first key.  This is equivalent to using a NULL cursor argument to
                // WT_SESSION->truncate, but in that case, truncate will need to open its own
                // cursor.  Since we already have a cursor, we can use it here to make the whole
                // operation faster.
                _positionAtFirstRecordId(opCtx, truncateStart, savedFirstKey, true);
                ret = session->truncate(session, nullptr, truncateStart, truncateEnd, nullptr);
            }

            if (ret == ENOENT || ret == WT_NOTFOUND) {
                // TODO we should remove this case once SERVER-17141 is resolved
                log() << "Soft failure truncating capped collection. Will try again later.";
                docsRemoved = 0;
            } else {
                invariantWTOK(ret);
                _changeNumRecords(opCtx, -docsRemoved);
                _increaseDataSize(opCtx, -sizeSaved);
                wuow.commit();
                // Save the key for the next round
                _cappedFirstRecord = firstRemainingId;
            }
        }
    } catch (const WriteConflictException&) {
        opCtx->releaseRecoveryUnit();
        opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(realRecoveryUnit), realRUstate);
        log() << "got conflict truncating capped, ignoring";
        return 0;
    } catch (...) {
        opCtx->releaseRecoveryUnit();
        opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(realRecoveryUnit), realRUstate);
        throw;
    }

    opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(realRecoveryUnit), realRUstate);
    return docsRemoved;
}

bool WiredTigerRecordStore::yieldAndAwaitOplogDeletionRequest(OperationContext* opCtx) {
    // Create another reference to the oplog stones while holding a lock on the collection to
    // prevent it from being destructed.
    std::shared_ptr<OplogStones> oplogStones = _oplogStones;

    Locker* locker = opCtx->lockState();
    Locker::LockSnapshot snapshot;

    // Release any locks before waiting on the condition variable. It is illegal to access any
    // methods or members of this record store after this line because it could be deleted.
    bool releasedAnyLocks = locker->saveLockStateAndUnlock(&snapshot);
    invariant(releasedAnyLocks);

    // The top-level locks were freed, so also release any potential low-level (storage engine)
    // locks that might be held.
    WiredTigerRecoveryUnit* recoveryUnit = (WiredTigerRecoveryUnit*)opCtx->recoveryUnit();
    recoveryUnit->abandonSnapshot();
    recoveryUnit->beginIdle();

    // Wait for an oplog deletion request, or for this record store to have been destroyed.
    oplogStones->awaitHasExcessStonesOrDead();

    // Reacquire the locks that were released.
    locker->restoreLockState(opCtx, snapshot);

    return !oplogStones->isDead();
}

void WiredTigerRecordStore::reclaimOplog(OperationContext* opCtx) {
    reclaimOplog(opCtx, _kvEngine->getPinnedOplog());
}

void WiredTigerRecordStore::reclaimOplog(OperationContext* opCtx, Timestamp mayTruncateUpTo) {
    Timer timer;
    while (auto stone = _oplogStones->peekOldestStoneIfNeeded()) {
        invariant(stone->lastRecord.isValid());

        if (static_cast<std::uint64_t>(stone->lastRecord.repr()) >= mayTruncateUpTo.asULL()) {
            // Do not truncate oplogs needed for replication recovery.
            return;
        }

        LOG(1) << "Truncating the oplog between " << _oplogStones->firstRecord << " and "
               << stone->lastRecord << " to remove approximately " << stone->records
               << " records totaling to " << stone->bytes << " bytes";

        WiredTigerRecoveryUnit* ru = WiredTigerRecoveryUnit::get(opCtx);
        WT_SESSION* session = ru->getSession()->getSession();

        try {
            WriteUnitOfWork wuow(opCtx);

            WiredTigerCursor cwrap(_uri, _tableId, true, opCtx);
            WT_CURSOR* cursor = cwrap.get();

            // The first record in the oplog should be within the truncate range.
            int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return cursor->next(cursor); });
            invariantWTOK(ret);
            RecordId firstRecord = getKey(cursor);
            if (firstRecord < _oplogStones->firstRecord || firstRecord > stone->lastRecord) {
                warning() << "First oplog record " << firstRecord << " is not in truncation range ("
                          << _oplogStones->firstRecord << ", " << stone->lastRecord << ")";
            }

            setKey(cursor, stone->lastRecord);
            invariantWTOK(session->truncate(session, nullptr, nullptr, cursor, nullptr));
            _changeNumRecords(opCtx, -stone->records);
            _increaseDataSize(opCtx, -stone->bytes);

            wuow.commit();

            // Remove the stone after a successful truncation.
            _oplogStones->popOldestStone();

            // Stash the truncate point for next time to cleanly skip over tombstones, etc.
            _oplogStones->firstRecord = stone->lastRecord;
        } catch (const WriteConflictException&) {
            LOG(1) << "Caught WriteConflictException while truncating oplog entries, retrying";
        }
    }

    LOG(1) << "Finished truncating the oplog, it now contains approximately "
           << _sizeInfo->numRecords.load() << " records totaling to " << _sizeInfo->dataSize.load()
           << " bytes";
    log() << "WiredTiger record store oplog truncation finished in: " << timer.millis() << "ms";
}

Status WiredTigerRecordStore::insertRecords(OperationContext* opCtx,
                                            std::vector<Record>* records,
                                            const std::vector<Timestamp>& timestamps) {
    return _insertRecords(opCtx, records->data(), timestamps.data(), records->size());
}

Status WiredTigerRecordStore::_insertRecords(OperationContext* opCtx,
                                             Record* records,
                                             const Timestamp* timestamps,
                                             size_t nRecords) {
    dassert(opCtx->lockState()->isWriteLocked());

    // We are kind of cheating on capped collections since we write all of them at once ....
    // Simplest way out would be to just block vector writes for everything except oplog ?
    int64_t totalLength = 0;
    for (size_t i = 0; i < nRecords; i++)
        totalLength += records[i].data.size();

    // caller will retry one element at a time
    if (_isCapped && totalLength > _cappedMaxSize)
        return Status(ErrorCodes::BadValue, "object to insert exceeds cappedMaxSize");

    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    RecordId highestId = RecordId();
    dassert(nRecords != 0);
    for (size_t i = 0; i < nRecords; i++) {
        auto& record = records[i];
        if (_isOplog) {
            StatusWith<RecordId> status =
                oploghack::extractKey(record.data.data(), record.data.size());
            if (!status.isOK())
                return status.getStatus();
            record.id = status.getValue();
        } else if (_isCapped) {
            record.id = _nextId();
        } else {
            record.id = _nextId();
        }
        dassert(record.id > highestId);
        highestId = record.id;
    }

    for (size_t i = 0; i < nRecords; i++) {
        auto& record = records[i];
        Timestamp ts;
        if (timestamps[i].isNull() && _isOplog) {
            // If the timestamp is 0, that probably means someone inserted a document directly
            // into the oplog.  In this case, use the RecordId as the timestamp, since they are
            // one and the same. Setting this transaction to be unordered will trigger a journal
            // flush. Because these are direct writes into the oplog, the machinery to trigger a
            // journal flush is bypassed. A followup oplog read will require a fresh visibility
            // value to make progress.
            ts = Timestamp(record.id.repr());
            opCtx->recoveryUnit()->setOrderedCommit(false);
        } else {
            ts = timestamps[i];
        }
        if (!ts.isNull()) {
            LOG(4) << "inserting record with timestamp " << ts;
            fassert(39001, opCtx->recoveryUnit()->setTimestamp(ts));
        }
        setKey(c, record.id);
        WiredTigerItem value(record.data.data(), record.data.size());
        c->set_value(c, value.Get());
        int ret = WT_OP_CHECK(c->insert(c));
        if (ret)
            return wtRCToStatus(ret, "WiredTigerRecordStore::insertRecord");
    }

    _changeNumRecords(opCtx, nRecords);
    _increaseDataSize(opCtx, totalLength);

    if (_oplogStones) {
        _oplogStones->updateCurrentStoneAfterInsertOnCommit(
            opCtx, totalLength, highestId, nRecords);
    } else {
        _cappedDeleteAsNeeded(opCtx, highestId);
    }

    return Status::OK();
}

bool WiredTigerRecordStore::isOpHidden_forTest(const RecordId& id) const {
    invariant(id.repr() > 0);
    invariant(_kvEngine->getOplogManager()->isRunning());
    return _kvEngine->getOplogManager()->getOplogReadTimestamp() <
        static_cast<std::uint64_t>(id.repr());
}

bool WiredTigerRecordStore::haveCappedWaiters() {
    stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
    return _cappedCallback && _cappedCallback->haveCappedWaiters();
}

void WiredTigerRecordStore::notifyCappedWaitersIfNeeded() {
    stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
    // This wakes up cursors blocking in await_data.
    if (_cappedCallback) {
        _cappedCallback->notifyCappedWaitersIfNeeded();
    }
}

Status WiredTigerRecordStore::insertRecordsWithDocWriter(OperationContext* opCtx,
                                                         const DocWriter* const* docs,
                                                         const Timestamp* timestamps,
                                                         size_t nDocs,
                                                         RecordId* idsOut) {
    dassert(opCtx->lockState()->isReadLocked());

    std::unique_ptr<Record[]> records(new Record[nDocs]);

    // First get all the sizes so we can allocate a single buffer for all documents. Eventually it
    // would be nice if we could either hand off the buffers to WT without copying or write them
    // in-place as we do with MMAPv1, but for now this is the best we can do.
    size_t totalSize = 0;
    for (size_t i = 0; i < nDocs; i++) {
        const size_t docSize = docs[i]->documentSize();
        records[i].data = RecordData(nullptr, docSize);  // We fill in the real ptr in next loop.
        totalSize += docSize;
    }

    std::unique_ptr<char[]> buffer(new char[totalSize]);
    char* pos = buffer.get();
    for (size_t i = 0; i < nDocs; i++) {
        docs[i]->writeDocument(pos);
        const size_t size = records[i].data.size();
        records[i].data = RecordData(pos, size);
        pos += size;
    }
    invariant(pos == (buffer.get() + totalSize));

    Status s = _insertRecords(opCtx, records.get(), timestamps, nDocs);
    if (!s.isOK())
        return s;

    if (idsOut) {
        for (size_t i = 0; i < nDocs; i++) {
            idsOut[i] = records[i].id;
        }
    }

    return s;
}

Status WiredTigerRecordStore::updateRecord(OperationContext* opCtx,
                                           const RecordId& id,
                                           const char* data,
                                           int len) {
    dassert(opCtx->lockState()->isWriteLocked());

    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    setKey(c, id);
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
    invariantWTOK(ret);

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret);

    int64_t old_length = old_value.size;

    if (_oplogStones && len != old_length) {
        return {ErrorCodes::IllegalOperation, "Cannot change the size of a document in the oplog"};
    }

    WiredTigerItem value(data, len);
    c->set_value(c, value.Get());
    ret = WT_OP_CHECK(c->insert(c));
    invariantWTOK(ret);

    _increaseDataSize(opCtx, len - old_length);
    if (!_oplogStones) {
        _cappedDeleteAsNeeded(opCtx, id);
    }

    return Status::OK();
}

bool WiredTigerRecordStore::updateWithDamagesSupported() const {
    return true;
}

StatusWith<RecordData> WiredTigerRecordStore::updateWithDamages(
    OperationContext* opCtx,
    const RecordId& id,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {

    const int nentries = damages.size();
    mutablebson::DamageVector::const_iterator where = damages.begin();
    const mutablebson::DamageVector::const_iterator end = damages.cend();
    std::vector<WT_MODIFY> entries(nentries);
    for (u_int i = 0; where != end; ++i, ++where) {
        entries[i].data.data = damageSource + where->sourceOffset;
        entries[i].data.size = where->size;
        entries[i].offset = where->targetOffset;
        entries[i].size = where->size;
    }

    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    setKey(c, id);

    // The test harness calls us with empty damage vectors which WiredTiger doesn't allow.
    if (nentries == 0)
        invariantWTOK(WT_OP_CHECK(c->search(c)));
    else
        invariantWTOK(WT_OP_CHECK(c->modify(c, entries.data(), nentries)));

    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value));

    return RecordData(static_cast<const char*>(value.data), value.size).getOwned();
}

std::unique_ptr<RecordCursor> WiredTigerRecordStore::getRandomCursor(
    OperationContext* opCtx) const {
    const char* extraConfig = "";
    return getRandomCursorWithOptions(opCtx, extraConfig);
}

Status WiredTigerRecordStore::truncate(OperationContext* opCtx) {
    WiredTigerCursor startWrap(_uri, _tableId, true, opCtx);
    WT_CURSOR* start = startWrap.get();
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return start->next(start); });
    // Empty collections don't have anything to truncate.
    if (ret == WT_NOTFOUND) {
        return Status::OK();
    }
    invariantWTOK(ret);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
    invariantWTOK(WT_OP_CHECK(session->truncate(session, NULL, start, NULL, NULL)));
    _changeNumRecords(opCtx, -numRecords(opCtx));
    _increaseDataSize(opCtx, -dataSize(opCtx));

    if (_oplogStones) {
        _oplogStones->clearStonesOnCommit(opCtx);
    }

    return Status::OK();
}

Status WiredTigerRecordStore::compact(OperationContext* opCtx) {
    dassert(opCtx->lockState()->isWriteLocked());

    WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache();
    if (!cache->isEphemeral()) {
        WT_SESSION* s = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
        opCtx->recoveryUnit()->abandonSnapshot();
        int ret = s->compact(s, getURI().c_str(), "timeout=0");
        invariantWTOK(ret);
    }
    return Status::OK();
}

Status WiredTigerRecordStore::validate(OperationContext* opCtx,
                                       ValidateCmdLevel level,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results,
                                       BSONObjBuilder* output) {
    dassert(opCtx->lockState()->isReadLocked());

    if (!_isEphemeral && level == kValidateFull) {
        int err = WiredTigerUtil::verifyTable(opCtx, _uri, &results->errors);
        if (err == EBUSY) {
            std::string msg = str::stream()
                << "Could not complete validation of " << _uri << ". "
                << "This is a transient issue as the collection was actively "
                   "in use by other operations.";

            warning() << msg;
            results->warnings.push_back(msg);
        } else if (err) {
            std::string msg = str::stream() << "verify() returned " << wiredtiger_strerror(err)
                                            << ". "
                                            << "This indicates structural damage. "
                                            << "Not examining individual documents.";
            error() << msg;
            results->errors.push_back(msg);
            results->valid = false;
            return Status::OK();
        }
    }

    long long nrecords = 0;
    long long dataSizeTotal = 0;
    long long nInvalid = 0;

    results->valid = true;
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, true);
    int interruptInterval = 4096;

    while (auto record = cursor->next()) {
        if (!(nrecords % interruptInterval))
            opCtx->checkForInterrupt();
        ++nrecords;
        auto dataSize = record->data.size();
        dataSizeTotal += dataSize;
        size_t validatedSize;
        Status status = adaptor->validate(record->id, record->data, &validatedSize);

        // The validatedSize equals dataSize below is not a general requirement, but must be
        // true for WT today because we never pad records.
        if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
            if (results->valid) {
                // Only log once.
                results->errors.push_back("detected one or more invalid documents (see logs)");
            }
            nInvalid++;
            results->valid = false;
            log() << "document at location: " << record->id << " is corrupted";
        }
    }

    if (results->valid) {
        updateStatsAfterRepair(opCtx, nrecords, dataSizeTotal);
    }

    output->append("nInvalidDocuments", nInvalid);
    output->appendNumber("nrecords", nrecords);
    return Status::OK();
}

void WiredTigerRecordStore::appendCustomStats(OperationContext* opCtx,
                                              BSONObjBuilder* result,
                                              double scale) const {
    result->appendBool("capped", _isCapped);
    if (_isCapped) {
        result->appendIntOrLL("max", _cappedMaxDocs);
        result->appendIntOrLL("maxSize", static_cast<long long>(_cappedMaxSize / scale));
        result->appendIntOrLL("sleepCount", _cappedSleep.load());
        result->appendIntOrLL("sleepMS", _cappedSleepMS.load());
    }
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();
    WT_SESSION* s = session->getSession();
    BSONObjBuilder bob(result->subobjStart(_engineName));
    {
        BSONObjBuilder metadata(bob.subobjStart("metadata"));
        Status status = WiredTigerUtil::getApplicationMetadata(opCtx, getURI(), &metadata);
        if (!status.isOK()) {
            metadata.append("error", "unable to retrieve metadata");
            metadata.append("code", static_cast<int>(status.code()));
            metadata.append("reason", status.reason());
        }
    }

    std::string type, sourceURI;
    WiredTigerUtil::fetchTypeAndSourceURI(opCtx, _uri, &type, &sourceURI);
    StatusWith<std::string> metadataResult = WiredTigerUtil::getMetadata(opCtx, sourceURI);
    StringData creationStringName("creationString");
    if (!metadataResult.isOK()) {
        BSONObjBuilder creationString(bob.subobjStart(creationStringName));
        creationString.append("error", "unable to retrieve creation config");
        creationString.append("code", static_cast<int>(metadataResult.getStatus().code()));
        creationString.append("reason", metadataResult.getStatus().reason());
    } else {
        bob.append("creationString", metadataResult.getValue());
        // Type can be "lsm" or "file"
        bob.append("type", type);
    }

    Status status =
        WiredTigerUtil::exportTableToBSON(s, "statistics:" + getURI(), "statistics=(fast)", &bob);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }
}

Status WiredTigerRecordStore::touch(OperationContext* opCtx, BSONObjBuilder* output) const {
    if (_isEphemeral) {
        // Everything is already in memory.
        return Status::OK();
    }
    return Status(ErrorCodes::CommandNotSupported, "this storage engine does not support touch");
}

void WiredTigerRecordStore::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const {
    // Make sure that callers do not hold an active snapshot so it will be able to see the oplog
    // entries it waited for afterwards.
    invariant(!_getRecoveryUnit(opCtx)->inActiveTxn());

    auto oplogManager = _kvEngine->getOplogManager();
    if (oplogManager->isRunning()) {
        oplogManager->waitForAllEarlierOplogWritesToBeVisible(this, opCtx);
    }
}

boost::optional<RecordId> WiredTigerRecordStore::oplogStartHack(
    OperationContext* opCtx, const RecordId& startingPosition) const {
    dassert(opCtx->lockState()->isReadLocked());

    if (!_isOplog)
        return boost::none;

    if (_isOplog) {
        WiredTigerRecoveryUnit::get(opCtx)->setIsOplogReader();
    }

    WiredTigerCursor cursor(_uri, _tableId, true, opCtx);
    WT_CURSOR* c = cursor.get();

    int cmp;
    setKey(c, startingPosition);
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search_near(c, &cmp); });
    if (ret == 0 && cmp > 0)
        ret = c->prev(c);  // landed one higher than startingPosition
    if (ret == WT_NOTFOUND)
        return RecordId();  // nothing <= startingPosition
    invariantWTOK(ret);

    return getKey(c);
}

void WiredTigerRecordStore::updateStatsAfterRepair(OperationContext* opCtx,
                                                   long long numRecords,
                                                   long long dataSize) {
    // We're correcting the size as of now, future writes should be tracked.
    sizeRecoveryState(getGlobalServiceContext()).markCollectionAsAlwaysNeedsSizeAdjustment(_ident);

    _sizeInfo->numRecords.store(numRecords);
    _sizeInfo->dataSize.store(dataSize);

    // If we have a WiredTigerSizeStorer, but our size info is not currently cached, add it.
    if (_sizeStorer)
        _sizeStorer->store(_uri, _sizeInfo);
}

RecordId WiredTigerRecordStore::_nextId() {
    invariant(!_isOplog);
    RecordId out = RecordId(_nextIdNum.fetchAndAdd(1));
    invariant(out.isNormal());
    return out;
}

WiredTigerRecoveryUnit* WiredTigerRecordStore::_getRecoveryUnit(OperationContext* opCtx) {
    return checked_cast<WiredTigerRecoveryUnit*>(opCtx->recoveryUnit());
}

class WiredTigerRecordStore::NumRecordsChange : public RecoveryUnit::Change {
public:
    NumRecordsChange(WiredTigerRecordStore* rs, int64_t diff) : _rs(rs), _diff(diff) {}
    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        LOG(3) << "WiredTigerRecordStore: rolling back NumRecordsChange" << -_diff;
        _rs->_sizeInfo->numRecords.fetchAndAdd(-_diff);
    }

private:
    WiredTigerRecordStore* _rs;
    int64_t _diff;
};

void WiredTigerRecordStore::_changeNumRecords(OperationContext* opCtx, int64_t diff) {
    if (!sizeRecoveryState(getGlobalServiceContext()).collectionNeedsSizeAdjustment(_ident)) {
        return;
    }

    opCtx->recoveryUnit()->registerChange(new NumRecordsChange(this, diff));
    if (_sizeInfo->numRecords.fetchAndAdd(diff) < 0)
        _sizeInfo->numRecords.store(std::max(diff, int64_t(0)));
}

class WiredTigerRecordStore::DataSizeChange : public RecoveryUnit::Change {
public:
    DataSizeChange(WiredTigerRecordStore* rs, int64_t amount) : _rs(rs), _amount(amount) {}
    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        _rs->_increaseDataSize(NULL, -_amount);
    }

private:
    WiredTigerRecordStore* _rs;
    int64_t _amount;
};

void WiredTigerRecordStore::_increaseDataSize(OperationContext* opCtx, int64_t amount) {
    if (!sizeRecoveryState(getGlobalServiceContext()).collectionNeedsSizeAdjustment(_ident)) {
        return;
    }

    if (opCtx)
        opCtx->recoveryUnit()->registerChange(new DataSizeChange(this, amount));

    if (_sizeInfo->dataSize.fetchAndAdd(amount) < 0)
        _sizeInfo->dataSize.store(std::max(amount, int64_t(0)));

    if (_sizeStorer)
        _sizeStorer->store(_uri, _sizeInfo);
}

void WiredTigerRecordStore::cappedTruncateAfter(OperationContext* opCtx,
                                                RecordId end,
                                                bool inclusive) {
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, true);

    auto record = cursor->seekExact(end);
    massert(28807, str::stream() << "Failed to seek to the record located at " << end, record);

    int64_t recordsRemoved = 0;
    int64_t bytesRemoved = 0;
    RecordId lastKeptId;
    RecordId firstRemovedId;

    if (inclusive) {
        std::unique_ptr<SeekableRecordCursor> reverseCursor = getCursor(opCtx, false);
        invariant(reverseCursor->seekExact(end));
        auto prev = reverseCursor->next();
        lastKeptId = prev ? prev->id : RecordId();
        firstRemovedId = end;
    } else {
        // If not deleting the record located at 'end', then advance the cursor to the first record
        // that is being deleted.
        record = cursor->next();
        if (!record) {
            return;  // No records to delete.
        }
        lastKeptId = end;
        firstRemovedId = record->id;
    }

    // Compute the number and associated sizes of the records to delete.
    {
        stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
        do {
            if (_cappedCallback) {
                uassertStatusOK(
                    _cappedCallback->aboutToDeleteCapped(opCtx, record->id, record->data));
            }
            recordsRemoved++;
            bytesRemoved += record->data.size();
        } while ((record = cursor->next()));
    }

    // Truncate the collection starting from the record located at 'firstRemovedId' to the end of
    // the collection.
    WriteUnitOfWork wuow(opCtx);

    WiredTigerCursor startwrap(_uri, _tableId, true, opCtx);
    WT_CURSOR* start = startwrap.get();
    setKey(start, firstRemovedId);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
    invariantWTOK(session->truncate(session, nullptr, start, nullptr, nullptr));

    _changeNumRecords(opCtx, -recordsRemoved);
    _increaseDataSize(opCtx, -bytesRemoved);

    wuow.commit();

    if (_isOplog) {
        // Immediately rewind visibility to our truncation point, to prevent new
        // transactions from appearing.
        Timestamp truncTs(lastKeptId.repr());

        if (!serverGlobalParams.enableMajorityReadConcern) {
            // If majority read concern is disabled, we must set the oldest timestamp along with the
            // commit timestamp. Otherwise, the commit timestamp might be set behind the oldest
            // timestamp.
            const bool force = true;
            _kvEngine->setOldestTimestamp(truncTs, force);
        } else {
            char commitTSConfigString["commit_timestamp="_sd.size() +
                                      (8 * 2) /* 8 hexadecimal characters */ +
                                      1 /* trailing null */];
            auto size = std::snprintf(commitTSConfigString,
                                      sizeof(commitTSConfigString),
                                      "commit_timestamp=%llx",
                                      truncTs.asULL());
            if (size < 0) {
                int e = errno;
                error() << "error snprintf " << errnoWithDescription(e);
                fassertFailedNoTrace(40662);
            }

            invariant(static_cast<std::size_t>(size) < sizeof(commitTSConfigString));
            auto conn = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn();
            invariantWTOK(conn->set_timestamp(conn, commitTSConfigString));
        }

        _kvEngine->getOplogManager()->setOplogReadTimestamp(truncTs);
        LOG(1) << "truncation new read timestamp: " << truncTs;
    }

    if (_oplogStones) {
        _oplogStones->updateStonesAfterCappedTruncateAfter(
            recordsRemoved, bytesRemoved, firstRemovedId);
    }
}

Status WiredTigerRecordStore::oplogDiskLocRegister(OperationContext* opCtx,
                                                   const Timestamp& ts,
                                                   bool orderedCommit) {
    opCtx->recoveryUnit()->setOrderedCommit(orderedCommit);
    if (!orderedCommit) {
        // This labels the current transaction with a timestamp.
        // This is required for oplog visibility to work correctly, as WiredTiger uses the
        // transaction list to determine where there are holes in the oplog.
        return opCtx->recoveryUnit()->setTimestamp(ts);
    }
    // This handles non-primary (secondary) state behavior; we simply set the oplog visiblity read
    // timestamp here, as there cannot be visible holes prior to the opTime passed in.
    _kvEngine->getOplogManager()->setOplogReadTimestamp(ts);
    return Status::OK();
}

// Cursor Base:

WiredTigerRecordStoreCursorBase::WiredTigerRecordStoreCursorBase(OperationContext* opCtx,
                                                                 const WiredTigerRecordStore& rs,
                                                                 bool forward)
    : _rs(rs), _opCtx(opCtx), _forward(forward) {
    _cursor.emplace(rs.getURI(), rs.tableId(), true, opCtx);
}

boost::optional<Record> WiredTigerRecordStoreCursorBase::next() {
    if (_eof)
        return {};

    WT_CURSOR* c = _cursor->get();

    RecordId id;
    if (!_skipNextAdvance) {
        // Nothing after the next line can throw WCEs.
        // Note that an unpositioned (or eof) WT_CURSOR returns the first/last entry in the
        // table when you call next/prev.
        int advanceRet = wiredTigerPrepareConflictRetry(
            _opCtx, [&] { return _forward ? c->next(c) : c->prev(c); });
        if (advanceRet == WT_NOTFOUND) {
            _eof = true;
            return {};
        }
        invariantWTOK(advanceRet);
        if (hasWrongPrefix(c, &id)) {
            _eof = true;
            return {};
        }
    }

    _skipNextAdvance = false;
    if (!id.isValid()) {
        id = getKey(c);
    }

    if (_forward && _lastReturnedId >= id) {
        log() << "WTCursor::next -- c->next_key ( " << id
              << ") was not greater than _lastReturnedId (" << _lastReturnedId
              << ") which is a bug.";
        // Force a retry of the operation from our last known position by acting as-if
        // we received a WT_ROLLBACK error.
        throw WriteConflictException();
    }

    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value));

    _lastReturnedId = id;
    return {{id, {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
}

boost::optional<Record> WiredTigerRecordStoreCursorBase::seekExact(const RecordId& id) {
    _skipNextAdvance = false;
    WT_CURSOR* c = _cursor->get();
    setKey(c, id);
    // Nothing after the next line can throw WCEs.
    int seekRet = wiredTigerPrepareConflictRetry(_opCtx, [&] { return c->search(c); });
    if (seekRet == WT_NOTFOUND) {
        // hasWrongPrefix check not needed for a precise 'WT_CURSOR::search'.
        _eof = true;
        return {};
    }
    invariantWTOK(seekRet);

    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value));

    _lastReturnedId = id;
    _eof = false;
    return {{id, {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
}


void WiredTigerRecordStoreCursorBase::save() {
    try {
        if (_cursor)
            _cursor->reset();
    } catch (const WriteConflictException&) {
        // Ignore since this is only called when we are about to kill our transaction
        // anyway.
    }
}

void WiredTigerRecordStoreCursorBase::saveUnpositioned() {
    save();
    _lastReturnedId = RecordId();
}

bool WiredTigerRecordStoreCursorBase::restore() {
    if (_rs._isOplog && _forward) {
        WiredTigerRecoveryUnit::get(_opCtx)->setIsOplogReader();
    }

    if (!_cursor)
        _cursor.emplace(_rs.getURI(), _rs.tableId(), true, _opCtx);

    // This will ensure an active session exists, so any restored cursors will bind to it
    invariant(WiredTigerRecoveryUnit::get(_opCtx)->getSession() == _cursor->getSession());
    _skipNextAdvance = false;

    // If we've hit EOF, then this iterator is done and need not be restored.
    if (_eof)
        return true;

    if (_lastReturnedId.isNull()) {
        initCursorToBeginning();
        return true;
    }

    WT_CURSOR* c = _cursor->get();
    setKey(c, _lastReturnedId);

    int cmp;
    int ret = wiredTigerPrepareConflictRetry(_opCtx, [&] { return c->search_near(c, &cmp); });
    RecordId id;
    if (ret == WT_NOTFOUND) {
        _eof = true;
        return !_rs._isCapped;
    }
    invariantWTOK(ret);
    if (hasWrongPrefix(c, &id)) {
        _eof = true;
        return !_rs._isCapped;
    }

    if (cmp == 0)
        return true;  // Landed right where we left off.

    if (_rs._isCapped) {
        // Doc was deleted either by _cappedDeleteAsNeeded() or cappedTruncateAfter().
        // It is important that we error out in this case so that consumers don't
        // silently get 'holes' when scanning capped collections. We don't make
        // this guarantee for normal collections so it is ok to skip ahead in that case.
        _eof = true;
        return false;
    }

    if (_forward && cmp > 0) {
        // We landed after where we were. Return our new location on the next call to next().
        _skipNextAdvance = true;
    } else if (!_forward && cmp < 0) {
        _skipNextAdvance = true;
    }

    return true;
}

void WiredTigerRecordStoreCursorBase::detachFromOperationContext() {
    _opCtx = nullptr;
    _cursor = boost::none;
}

void WiredTigerRecordStoreCursorBase::reattachToOperationContext(OperationContext* opCtx) {
    _opCtx = opCtx;
    // _cursor recreated in restore() to avoid risk of WT_ROLLBACK issues.
}

// Standard Implementations:


StandardWiredTigerRecordStore::StandardWiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                                             OperationContext* opCtx,
                                                             Params params)
    : WiredTigerRecordStore(kvEngine, opCtx, params) {}

RecordId StandardWiredTigerRecordStore::getKey(WT_CURSOR* cursor) const {
    std::int64_t recordId;
    invariantWTOK(cursor->get_key(cursor, &recordId));
    return RecordId(recordId);
}

void StandardWiredTigerRecordStore::setKey(WT_CURSOR* cursor, RecordId id) const {
    cursor->set_key(cursor, id.repr());
}

std::unique_ptr<SeekableRecordCursor> StandardWiredTigerRecordStore::getCursor(
    OperationContext* opCtx, bool forward) const {
    dassert(opCtx->lockState()->isReadLocked());

    if (_isOplog && forward) {
        WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(opCtx);
        // If we already have a snapshot we don't know what it can see, unless we know no one
        // else could be writing (because we hold an exclusive lock).
        if (wru->inActiveTxn() && !opCtx->lockState()->isNoop() &&
            !opCtx->lockState()->isCollectionLockedForMode(_ns, MODE_X)) {
            throw WriteConflictException();
        }
        wru->setIsOplogReader();
    }

    return stdx::make_unique<WiredTigerRecordStoreStandardCursor>(opCtx, *this, forward);
}

std::unique_ptr<RecordCursor> StandardWiredTigerRecordStore::getRandomCursorWithOptions(
    OperationContext* opCtx, StringData extraConfig) const {
    return stdx::make_unique<RandomCursor>(opCtx, *this, extraConfig);
}

WiredTigerRecordStoreStandardCursor::WiredTigerRecordStoreStandardCursor(
    OperationContext* opCtx, const WiredTigerRecordStore& rs, bool forward)
    : WiredTigerRecordStoreCursorBase(opCtx, rs, forward) {}

void WiredTigerRecordStoreStandardCursor::setKey(WT_CURSOR* cursor, RecordId id) const {
    cursor->set_key(cursor, id.repr());
}

RecordId WiredTigerRecordStoreStandardCursor::getKey(WT_CURSOR* cursor) const {
    std::int64_t recordId;
    invariantWTOK(cursor->get_key(cursor, &recordId));

    return RecordId(recordId);
}

bool WiredTigerRecordStoreStandardCursor::hasWrongPrefix(WT_CURSOR* cursor,
                                                         RecordId* recordId) const {
    invariantWTOK(cursor->get_key(cursor, recordId));
    return false;
}


// Prefixed Implementations:

PrefixedWiredTigerRecordStore::PrefixedWiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                                             OperationContext* opCtx,
                                                             Params params,
                                                             KVPrefix prefix)
    : WiredTigerRecordStore(kvEngine, opCtx, params), _prefix(prefix) {}

std::unique_ptr<SeekableRecordCursor> PrefixedWiredTigerRecordStore::getCursor(
    OperationContext* opCtx, bool forward) const {
    dassert(opCtx->lockState()->isReadLocked());

    if (_isOplog && forward) {
        WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(opCtx);
        // If we already have a snapshot we don't know what it can see, unless we know no one
        // else could be writing (because we hold an exclusive lock).
        if (wru->inActiveTxn() && !opCtx->lockState()->isNoop() &&
            !opCtx->lockState()->isCollectionLockedForMode(_ns, MODE_X)) {
            throw WriteConflictException();
        }
        wru->setIsOplogReader();
    }

    return stdx::make_unique<WiredTigerRecordStorePrefixedCursor>(opCtx, *this, _prefix, forward);
}

std::unique_ptr<RecordCursor> PrefixedWiredTigerRecordStore::getRandomCursorWithOptions(
    OperationContext* opCtx, StringData extraConfig) const {
    return {};
}

RecordId PrefixedWiredTigerRecordStore::getKey(WT_CURSOR* cursor) const {
    std::int64_t prefix;
    std::int64_t recordId;
    invariantWTOK(cursor->get_key(cursor, &prefix, &recordId));
    invariant(prefix == _prefix.repr());
    return RecordId(recordId);
}

void PrefixedWiredTigerRecordStore::setKey(WT_CURSOR* cursor, RecordId id) const {
    cursor->set_key(cursor, _prefix.repr(), id.repr());
}

WiredTigerRecordStorePrefixedCursor::WiredTigerRecordStorePrefixedCursor(
    OperationContext* opCtx, const WiredTigerRecordStore& rs, KVPrefix prefix, bool forward)
    : WiredTigerRecordStoreCursorBase(opCtx, rs, forward), _prefix(prefix) {
    initCursorToBeginning();
}

void WiredTigerRecordStorePrefixedCursor::setKey(WT_CURSOR* cursor, RecordId id) const {
    cursor->set_key(cursor, _prefix.repr(), id.repr());
}

RecordId WiredTigerRecordStorePrefixedCursor::getKey(WT_CURSOR* cursor) const {
    std::int64_t prefix;
    std::int64_t recordId;
    invariantWTOK(cursor->get_key(cursor, &prefix, &recordId));
    invariant(prefix == _prefix.repr());

    return RecordId(recordId);
}

bool WiredTigerRecordStorePrefixedCursor::hasWrongPrefix(WT_CURSOR* cursor,
                                                         RecordId* recordId) const {
    std::int64_t prefix;
    invariantWTOK(cursor->get_key(cursor, &prefix, recordId));

    return prefix != _prefix.repr();
}

void WiredTigerRecordStorePrefixedCursor::initCursorToBeginning() {
    WT_CURSOR* cursor = _cursor->get();
    if (_forward) {
        cursor->set_key(cursor, _prefix.repr(), RecordId::min());
    } else {
        cursor->set_key(cursor, _prefix.repr(), RecordId::max());
    }

    int exact;
    int err = cursor->search_near(cursor, &exact);
    if (err == WT_NOTFOUND) {
        _eof = true;
        return;
    }
    invariantWTOK(err);

    RecordId recordId;
    if (_forward) {
        invariant(exact != 0);  // `RecordId::min` cannot exist.
        if (exact > 0) {
            // Cursor is positioned after <Prefix, RecordId::min>. It may be the first record of
            // this collection or a following collection with a larger prefix.
            //
            // In the case the cursor is positioned a matching prefix, `_skipNextAdvance` must
            // be set to true. However, `WiredTigerRecordStore::Cursor::next` does not check
            // for EOF if `_skipNextAdvance` is true. Eagerly check and set `_eof` if
            // necessary.
            if (hasWrongPrefix(cursor, &recordId)) {
                _eof = true;
                return;
            }

            _skipNextAdvance = true;
        } else {
            _eof = true;
        }
    } else {                    // Backwards.
        invariant(exact != 0);  // `RecordId::min` cannot exist.
        if (exact > 0) {
            // Cursor is positioned after <Prefix, RecordId::max>. This implies it is
            // positioned at the first record for a collection with a larger
            // prefix. `_skipNextAdvance` should remain false and a following call to
            // `WiredTigerRecordStore::Cursor::next` will advance the cursor and appropriately
            // check for EOF.
            _skipNextAdvance = false;  // Simply for clarity and symmetry to the `forward` case.
        } else {
            // Cursor is positioned before <Prefix, RecordId::max>. This is a symmetric case
            // to `forward: true, exact > 0`. It may be positioned at the last document of
            // this collection or the last document of a collection with a smaller prefix.
            if (hasWrongPrefix(cursor, &recordId)) {
                _eof = true;
                return;
            }

            _skipNextAdvance = true;
        }
    }
}

Status WiredTigerRecordStore::updateCappedSize(OperationContext* opCtx, long long cappedSize) {
    if (_cappedMaxSize == cappedSize) {
        return Status::OK();
    }
    _cappedMaxSize = cappedSize;
    if (_oplogStones) {
        _oplogStones->adjust(cappedSize);
    }
    return Status::OK();
}

}  // namespace mongo
