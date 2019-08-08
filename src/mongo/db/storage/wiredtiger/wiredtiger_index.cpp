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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"

#include <memory>
#include <set>

#include "mongo/base/checked_cast.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/json.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

#define TRACING_ENABLED 0

#if TRACING_ENABLED
#define TRACE_CURSOR log() << "WT index (" << (const void*)&_idx << ") "
#define TRACE_INDEX log() << "WT index (" << (const void*)this << ") "
#else
#define TRACE_CURSOR \
    if (0)           \
    log()
#define TRACE_INDEX \
    if (0)          \
    log()
#endif

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(WTEmulateOutOfOrderNextIndexKey);

using std::string;
using std::vector;

static const WiredTigerItem emptyItem(nullptr, 0);
}  // namespace


// Keystring format 7 was used in 3.3.6 - 3.3.8 development releases. 4.2 onwards, unique indexes
// can be either format version 11 or 12. On upgrading to 4.2, an existing format 6 unique index
// will upgrade to format 11 and an existing format 8 unique index will upgrade to format 12.
const int kDataFormatV1KeyStringV0IndexVersionV1 = 6;
const int kDataFormatV2KeyStringV1IndexVersionV2 = 8;
const int kDataFormatV3KeyStringV0UniqueIndexVersionV1 = 11;
const int kDataFormatV4KeyStringV1UniqueIndexVersionV2 = 12;
const int kMinimumIndexVersion = kDataFormatV1KeyStringV0IndexVersionV1;
const int kMaximumIndexVersion = kDataFormatV4KeyStringV1UniqueIndexVersionV2;

void WiredTigerIndex::setKey(WT_CURSOR* cursor, const WT_ITEM* item) {
    if (_prefix == KVPrefix::kNotPrefixed) {
        cursor->set_key(cursor, item);
    } else {
        cursor->set_key(cursor, _prefix.repr(), item);
    }
}

void WiredTigerIndex::getKey(WT_CURSOR* cursor, WT_ITEM* key) {
    if (_prefix == KVPrefix::kNotPrefixed) {
        invariantWTOK(cursor->get_key(cursor, key));
    } else {
        int64_t prefix;
        invariantWTOK(cursor->get_key(cursor, &prefix, key));
        invariant(_prefix.repr() == prefix);
    }
}

// static
StatusWith<std::string> WiredTigerIndex::parseIndexOptions(const BSONObj& options) {
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
                                                         << '\'' << " is not a supported option.");
        }
    }
    return StatusWith<std::string>(ss.str());
}

// static
std::string WiredTigerIndex::generateAppMetadataString(const IndexDescriptor& desc) {
    StringBuilder ss;

    int keyStringVersion;

    if (desc.unique() && !desc.isIdIndex()) {
        keyStringVersion = desc.version() >= IndexDescriptor::IndexVersion::kV2
            ? kDataFormatV4KeyStringV1UniqueIndexVersionV2
            : kDataFormatV3KeyStringV0UniqueIndexVersionV1;
    } else {
        keyStringVersion = desc.version() >= IndexDescriptor::IndexVersion::kV2
            ? kDataFormatV2KeyStringV1IndexVersionV2
            : kDataFormatV1KeyStringV0IndexVersionV1;
    }

    // Index metadata
    ss << ",app_metadata=("
       << "formatVersion=" << keyStringVersion << "),";

    return (ss.str());
}

// static
StatusWith<std::string> WiredTigerIndex::generateCreateString(const std::string& engineName,
                                                              const std::string& sysIndexConfig,
                                                              const std::string& collIndexConfig,
                                                              const IndexDescriptor& desc,
                                                              bool isPrefixed) {
    str::stream ss;

    // Separate out a prefix and suffix in the default string. User configuration will override
    // values in the prefix, but not values in the suffix.  Page sizes are chosen so that index
    // keys (up to 1024 bytes) will not overflow.
    ss << "type=file,internal_page_max=16k,leaf_page_max=16k,";
    ss << "checksum=on,";
    if (wiredTigerGlobalOptions.useIndexPrefixCompression) {
        ss << "prefix_compression=true,";
    }

    ss << "block_compressor=" << wiredTigerGlobalOptions.indexBlockCompressor << ",";
    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig(desc.parentNS().ns());
    ss << sysIndexConfig << ",";
    ss << collIndexConfig << ",";

    // Validate configuration object.
    // Raise an error about unrecognized fields that may be introduced in newer versions of
    // this storage engine.
    // Ensure that 'configString' field is a string. Raise an error if this is not the case.
    BSONElement storageEngineElement = desc.infoObj()["storageEngine"];
    if (storageEngineElement.isABSONObj()) {
        BSONObj storageEngine = storageEngineElement.Obj();
        StatusWith<std::string> parseStatus =
            parseIndexOptions(storageEngine.getObjectField(engineName));
        if (!parseStatus.isOK()) {
            return parseStatus;
        }
        if (!parseStatus.getValue().empty()) {
            ss << "," << parseStatus.getValue();
        }
    }

    // WARNING: No user-specified config can appear below this line. These options are required
    // for correct behavior of the server.

    // Indexes need to store the metadata for collation to work as expected.
    if (isPrefixed) {
        ss << ",key_format=qu";
    } else {
        ss << ",key_format=u";
    }
    ss << ",value_format=u";

    // Index metadata
    ss << generateAppMetadataString(desc);

    bool replicatedWrites = getGlobalReplSettings().usingReplSets() ||
        repl::ReplSettings::shouldRecoverFromOplogAsStandalone();
    if (WiredTigerUtil::useTableLogging(NamespaceString(desc.parentNS()), replicatedWrites)) {
        ss << "log=(enabled=true)";
    } else {
        ss << "log=(enabled=false)";
    }

    LOG(3) << "index create string: " << ss.ss.str();
    return StatusWith<std::string>(ss);
}

int WiredTigerIndex::Create(OperationContext* opCtx,
                            const std::string& uri,
                            const std::string& config) {
    // Don't use the session from the recovery unit: create should not be used in a transaction
    WiredTigerSession session(WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn());
    WT_SESSION* s = session.getSession();
    LOG(1) << "create uri: " << uri << " config: " << config;
    return s->create(s, uri.c_str(), config.c_str());
}

WiredTigerIndex::WiredTigerIndex(OperationContext* ctx,
                                 const std::string& uri,
                                 const IndexDescriptor* desc,
                                 KVPrefix prefix,
                                 bool isReadOnly)
    : SortedDataInterface(_handleVersionInfo(ctx, uri, desc, isReadOnly),
                          Ordering::make(desc->keyPattern())),
      _uri(uri),
      _tableId(WiredTigerSession::genTableId()),
      _collectionNamespace(desc->parentNS()),
      _indexName(desc->indexName()),
      _keyPattern(desc->keyPattern()),
      _prefix(prefix),
      _isIdIndex(desc->isIdIndex()) {}

Status WiredTigerIndex::insert(OperationContext* opCtx,
                               const BSONObj& key,
                               const RecordId& id,
                               bool dupsAllowed) {
    dassert(opCtx->lockState()->isWriteLocked());
    invariant(id.isValid());
    dassert(!key.hasFieldNames());

    TRACE_INDEX << " key: " << key << " id: " << id;

    KeyString::HeapBuilder keyString(getKeyStringVersion(), key, _ordering, id);

    return insert(opCtx, std::move(keyString.release()), id, dupsAllowed);
}

Status WiredTigerIndex::insert(OperationContext* opCtx,
                               const KeyString::Value& keyString,
                               const RecordId& id,
                               bool dupsAllowed) {
    dassert(opCtx->lockState()->isWriteLocked());
    dassert(id == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

    TRACE_INDEX << " KeyString: " << keyString;

    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();

    return _insert(opCtx, c, keyString, id, dupsAllowed);
}

void WiredTigerIndex::unindex(OperationContext* opCtx,
                              const BSONObj& key,
                              const RecordId& id,
                              bool dupsAllowed) {
    invariant(id.isValid());
    dassert(!key.hasFieldNames());
    KeyString::HeapBuilder keyString(getKeyStringVersion(), key, _ordering, id);

    unindex(opCtx, std::move(keyString.release()), id, dupsAllowed);
}

void WiredTigerIndex::unindex(OperationContext* opCtx,
                              const KeyString::Value& keyString,
                              const RecordId& id,
                              bool dupsAllowed) {
    dassert(opCtx->lockState()->isWriteLocked());
    dassert(id == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    _unindex(opCtx, c, keyString, id, dupsAllowed);
}

void WiredTigerIndex::fullValidate(OperationContext* opCtx,
                                   long long* numKeysOut,
                                   ValidateResults* fullResults) const {
    dassert(opCtx->lockState()->isReadLocked());
    if (fullResults && !WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->isEphemeral()) {
        int err = WiredTigerUtil::verifyTable(opCtx, _uri, &(fullResults->errors));
        if (err == EBUSY) {
            std::string msg = str::stream()
                << "Could not complete validation of " << _uri << ". "
                << "This is a transient issue as the collection was actively "
                   "in use by other operations.";

            warning() << msg;
            fullResults->warnings.push_back(msg);
        } else if (err) {
            std::string msg = str::stream()
                << "verify() returned " << wiredtiger_strerror(err) << ". "
                << "This indicates structural damage. "
                << "Not examining individual index entries.";
            error() << msg;
            fullResults->errors.push_back(msg);
            fullResults->valid = false;
            return;
        }
    }

    auto cursor = newCursor(opCtx);
    long long count = 0;
    TRACE_INDEX << " fullValidate";

    const auto requestedInfo = TRACING_ENABLED ? Cursor::kKeyAndLoc : Cursor::kJustExistance;
    for (auto kv = cursor->seek(BSONObj(), true, requestedInfo); kv; kv = cursor->next()) {
        TRACE_INDEX << "\t" << kv->key << ' ' << kv->loc;
        count++;
    }
    if (numKeysOut) {
        *numKeysOut = count;
    }
}

bool WiredTigerIndex::appendCustomStats(OperationContext* opCtx,
                                        BSONObjBuilder* output,
                                        double scale) const {
    dassert(opCtx->lockState()->isReadLocked());
    {
        BSONObjBuilder metadata(output->subobjStart("metadata"));
        Status status = WiredTigerUtil::getApplicationMetadata(opCtx, uri(), &metadata);
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
        BSONObjBuilder creationString(output->subobjStart(creationStringName));
        creationString.append("error", "unable to retrieve creation config");
        creationString.append("code", static_cast<int>(metadataResult.getStatus().code()));
        creationString.append("reason", metadataResult.getStatus().reason());
    } else {
        output->append(creationStringName, metadataResult.getValue());
        // Type can be "lsm" or "file"
        output->append("type", type);
    }

    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSession();
    WT_SESSION* s = session->getSession();
    Status status =
        WiredTigerUtil::exportTableToBSON(s, "statistics:" + uri(), "statistics=(fast)", output);
    if (!status.isOK()) {
        output->append("error", "unable to retrieve statistics");
        output->append("code", static_cast<int>(status.code()));
        output->append("reason", status.reason());
    }
    return true;
}

Status WiredTigerIndex::dupKeyCheck(OperationContext* opCtx, const BSONObj& key) {
    invariant(!key.hasFieldNames());
    invariant(unique());

    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    WT_CURSOR* c = curwrap.get();

    if (isDup(opCtx, c, key))
        return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
    return Status::OK();
}

bool WiredTigerIndex::isEmpty(OperationContext* opCtx) {
    if (_prefix != KVPrefix::kNotPrefixed) {
        const bool forward = true;
        auto cursor = newCursor(opCtx, forward);
        const bool inclusive = false;
        return cursor->seek(kMinBSONKey, inclusive, Cursor::RequestedInfo::kJustExistance) ==
            boost::none;
    }

    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    WT_CURSOR* c = curwrap.get();
    if (!c)
        return true;
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->next(c); });
    if (ret == WT_NOTFOUND)
        return true;
    invariantWTOK(ret);
    return false;
}

Status WiredTigerIndex::touch(OperationContext* opCtx) const {
    if (WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->isEphemeral()) {
        // Everything is already in memory.
        return Status::OK();
    }
    return Status(ErrorCodes::CommandNotSupported, "this storage engine does not support touch");
}


long long WiredTigerIndex::getSpaceUsedBytes(OperationContext* opCtx) const {
    dassert(opCtx->lockState()->isReadLocked());
    auto ru = WiredTigerRecoveryUnit::get(opCtx);
    WiredTigerSession* session = ru->getSession();

    if (ru->getSessionCache()->isEphemeral()) {
        // For ephemeral case, use cursor statistics
        const auto statsUri = "statistics:" + uri();

        // Helper function to retrieve stats and check for errors
        auto getStats = [&](int key) -> int64_t {
            auto result = WiredTigerUtil::getStatisticsValue(
                session->getSession(), statsUri, "statistics=(fast)", key);
            if (!result.isOK()) {
                if (result.getStatus().code() == ErrorCodes::CursorNotFound)
                    return 0;  // ident gone, so return 0

                uassertStatusOK(result.getStatus());
            }
            return result.getValue();
        };

        auto inserts = getStats(WT_STAT_DSRC_CURSOR_INSERT);
        auto removes = getStats(WT_STAT_DSRC_CURSOR_REMOVE);
        auto insertBytes = getStats(WT_STAT_DSRC_CURSOR_INSERT_BYTES);

        if (inserts == 0 || removes >= inserts)
            return 0;

        // Rough approximation of index size as average entry size times number of entries.
        // May be off if key sizes change significantly over the life time of the collection,
        // but is the best we can do currrently with the statistics available.
        auto bytesPerEntry = (insertBytes + inserts - 1) / inserts;  // round up
        auto numEntries = inserts - removes;
        return numEntries * bytesPerEntry;
    }

    return static_cast<long long>(WiredTigerUtil::getIdentSize(session->getSession(), _uri));
}

bool WiredTigerIndex::isDup(OperationContext* opCtx, WT_CURSOR* c, const BSONObj& key) {
    dassert(opCtx->lockState()->isReadLocked());
    invariant(unique());

    // First check whether the key exists.
    KeyString::Builder data(getKeyStringVersion(), key, _ordering);
    WiredTigerItem item(data.getBuffer(), data.getSize());
    setKey(c, item.Get());

    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
    if (ret == WT_NOTFOUND) {
        return false;
    }
    invariantWTOK(ret);

    // If the key exists, check if we already have this id at this key. If so, we don't
    // consider that to be a dup.
    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value));
    BufReader br(value.data, value.size);
    int records = 0;
    while (br.remaining()) {
        KeyString::decodeRecordId(&br);
        records++;

        KeyString::TypeBits::fromBuffer(getKeyStringVersion(), &br);  // Just advance the reader.
    }
    return records > 1;
}

Status WiredTigerIndex::initAsEmpty(OperationContext* opCtx) {
    // No-op
    return Status::OK();
}

Status WiredTigerIndex::compact(OperationContext* opCtx) {
    dassert(opCtx->lockState()->isWriteLocked());
    WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache();
    if (!cache->isEphemeral()) {
        WT_SESSION* s = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
        opCtx->recoveryUnit()->abandonSnapshot();
        int ret = s->compact(s, uri().c_str(), "timeout=0");
        invariantWTOK(ret);
    }
    return Status::OK();
}

KeyString::Version WiredTigerIndex::_handleVersionInfo(OperationContext* ctx,
                                                       const std::string& uri,
                                                       const IndexDescriptor* desc,
                                                       bool isReadOnly) {
    auto version = WiredTigerUtil::checkApplicationMetadataFormatVersion(
        ctx, uri, kMinimumIndexVersion, kMaximumIndexVersion);
    if (!version.isOK()) {
        Status versionStatus = version.getStatus();
        Status indexVersionStatus(ErrorCodes::UnsupportedFormat,
                                  str::stream()
                                      << versionStatus.reason() << " Index: {name: "
                                      << desc->indexName() << ", ns: " << desc->parentNS()
                                      << "} - version either too old or too new for this mongod.");
        fassertFailedWithStatusNoTrace(28579, indexVersionStatus);
    }
    _dataFormatVersion = version.getValue();

    if (!desc->isIdIndex() && desc->unique()) {
        Status versionStatus = _dataFormatVersion == kDataFormatV3KeyStringV0UniqueIndexVersionV1 ||
                _dataFormatVersion == kDataFormatV4KeyStringV1UniqueIndexVersionV2
            ? Status::OK()
            : Status(ErrorCodes::UnsupportedFormat,
                     str::stream()
                         << "Index: {name: " << desc->indexName() << ", ns: " << desc->parentNS()
                         << "} has incompatible format version: " << _dataFormatVersion
                         << ". MongoDB 4.2 onwards, WT secondary unique indexes use "
                            "either format version 11 or 12. See "
                            "https://dochub.mongodb.org/core/upgrade-4.2-procedures for "
                            "detailed instructions on upgrading the index format.");
        fassertNoTrace(31179, versionStatus);
    }

    if (!isReadOnly) {
        bool replicatedWrites = getGlobalReplSettings().usingReplSets() ||
            repl::ReplSettings::shouldRecoverFromOplogAsStandalone();
        uassertStatusOK(WiredTigerUtil::setTableLogging(
            ctx,
            uri,
            WiredTigerUtil::useTableLogging(NamespaceString(desc->parentNS()), replicatedWrites)));
    }

    /*
     * Index data format 6 and 11 correspond to KeyString version V0 and data format 8 and 12
     * correspond to KeyString version V1.
     */
    return (_dataFormatVersion == kDataFormatV2KeyStringV1IndexVersionV2 ||
            _dataFormatVersion == kDataFormatV4KeyStringV1UniqueIndexVersionV2)
        ? KeyString::Version::V1
        : KeyString::Version::V0;
}

/**
 * Base class for WiredTigerIndex bulk builders.
 *
 * Manages the bulk cursor used by bulk builders.
 */
class WiredTigerIndex::BulkBuilder : public SortedDataBuilderInterface {
public:
    BulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx, KVPrefix prefix)
        : _ordering(idx->_ordering),
          _opCtx(opCtx),
          _session(WiredTigerRecoveryUnit::get(_opCtx)->getSessionCache()->getSession()),
          _cursor(openBulkCursor(idx)),
          _prefix(prefix) {}

    ~BulkBuilder() {
        _cursor->close(_cursor);
    }

protected:
    WT_CURSOR* openBulkCursor(WiredTigerIndex* idx) {
        // Open cursors can cause bulk open_cursor to fail with EBUSY.
        // TODO any other cases that could cause EBUSY?
        WiredTigerSession* outerSession = WiredTigerRecoveryUnit::get(_opCtx)->getSession();
        outerSession->closeAllCursors(idx->uri());

        // Not using cursor cache since we need to set "bulk".
        WT_CURSOR* cursor;
        // Use a different session to ensure we don't hijack an existing transaction.
        // Configure the bulk cursor open to fail quickly if it would wait on a checkpoint
        // completing - since checkpoints can take a long time, and waiting can result in
        // an unexpected pause in building an index.
        WT_SESSION* session = _session->getSession();
        int err = session->open_cursor(
            session, idx->uri().c_str(), nullptr, "bulk,checkpoint_wait=false", &cursor);
        if (!err)
            return cursor;

        warning() << "failed to create WiredTiger bulk cursor: " << wiredtiger_strerror(err);
        warning() << "falling back to non-bulk cursor for index " << idx->uri();

        invariantWTOK(session->open_cursor(session, idx->uri().c_str(), nullptr, nullptr, &cursor));
        return cursor;
    }

    void setKey(WT_CURSOR* cursor, const WT_ITEM* item) {
        if (_prefix == KVPrefix::kNotPrefixed) {
            cursor->set_key(cursor, item);
        } else {
            cursor->set_key(cursor, _prefix.repr(), item);
        }
    }

    const Ordering _ordering;
    OperationContext* const _opCtx;
    UniqueWiredTigerSession const _session;
    WT_CURSOR* const _cursor;
    KVPrefix _prefix;
};

/**
 * Bulk builds a non-unique index.
 */
class WiredTigerIndex::StandardBulkBuilder : public BulkBuilder {
public:
    StandardBulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx, KVPrefix prefix)
        : BulkBuilder(idx, opCtx, prefix), _idx(idx) {}

    Status addKey(const BSONObj& key, const RecordId& id) override {
        KeyString::HeapBuilder keyString(_idx->getKeyStringVersion(), key, _idx->_ordering, id);

        return addKey(std::move(keyString.release()), id);
    }

    Status addKey(const KeyString::Value& keyString, const RecordId& id) override {
        dassert(id == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

        // Can't use WiredTigerCursor since we aren't using the cache.
        WiredTigerItem item(keyString.getBuffer(), keyString.getSize());
        setKey(_cursor, item.Get());

        WiredTigerItem valueItem = keyString.getTypeBits().isAllZeros()
            ? emptyItem
            : WiredTigerItem(keyString.getTypeBits().getBuffer(),
                             keyString.getTypeBits().getSize());

        _cursor->set_value(_cursor, valueItem.Get());

        invariantWTOK(_cursor->insert(_cursor));

        return Status::OK();
    }

    void commit(bool mayInterrupt) override {
        // TODO do we still need this?
        // this is bizarre, but required as part of the contract
        WriteUnitOfWork uow(_opCtx);
        uow.commit();
    }

private:
    WiredTigerIndex* _idx;
};

/**
 * Bulk builds a unique index.
 *
 * In order to support unique indexes in dupsAllowed mode this class only does an actual insert
 * after it sees a key after the one we are trying to insert. This allows us to gather up all
 * duplicate ids and insert them all together. This is necessary since bulk cursors can only
 * append data.
 */
class WiredTigerIndex::UniqueBulkBuilder : public BulkBuilder {
public:
    UniqueBulkBuilder(WiredTigerIndex* idx,
                      OperationContext* opCtx,
                      bool dupsAllowed,
                      KVPrefix prefix)
        : BulkBuilder(idx, opCtx, prefix),
          _idx(idx),
          _dupsAllowed(dupsAllowed),
          _previousKeyString(idx->getKeyStringVersion()) {}

    Status addKey(const BSONObj& newKey, const RecordId& id) override {
        KeyString::HeapBuilder newKeyString(
            _idx->getKeyStringVersion(), newKey, _idx->getOrdering(), id);
        return addKey(std::move(newKeyString.release()), id);
    }

    Status addKey(const KeyString::Value& newKeyString, const RecordId& id) override {
        dassert(id ==
                KeyString::decodeRecordIdAtEnd(newKeyString.getBuffer(), newKeyString.getSize()));

        if (_idx->isTimestampSafeUniqueIdx()) {
            return addKeyTimestampSafe(newKeyString);
        }
        return addKeyTimestampUnsafe(newKeyString, id);
    }

    void commit(bool mayInterrupt) override {
        WriteUnitOfWork uow(_opCtx);
        if (!_records.empty()) {
            // This handles inserting the last unique key.
            doInsert();
        }
        uow.commit();
    }

private:
    Status addKeyTimestampSafe(const KeyString::Value& newKeyString) {
        // Do a duplicate check, but only if dups aren't allowed.
        if (!_dupsAllowed) {
            const int cmp = newKeyString.compareWithoutRecordId(_previousKeyString);
            if (cmp == 0) {
                // Duplicate found!
                auto newKey = KeyString::toBson(newKeyString, _idx->_ordering);
                return buildDupKeyErrorStatus(
                    newKey, _idx->collectionNamespace(), _idx->indexName(), _idx->keyPattern());
            } else {
                /*
                 * _previousKeyString.isEmpty() is only true on the first call to addKey().
                 * newKeyString must be greater than previous key.
                 */
                invariant(_previousKeyString.isEmpty() || cmp > 0);
            }
        }

        // Can't use WiredTigerCursor since we aren't using the cache.
        WiredTigerItem keyItem(newKeyString.getBuffer(), newKeyString.getSize());
        setKey(_cursor, keyItem.Get());

        WiredTigerItem valueItem = newKeyString.getTypeBits().isAllZeros()
            ? emptyItem
            : WiredTigerItem(newKeyString.getTypeBits().getBuffer(),
                             newKeyString.getTypeBits().getSize());

        _cursor->set_value(_cursor, valueItem.Get());

        invariantWTOK(_cursor->insert(_cursor));

        // Don't copy the key again if dups are allowed.
        if (!_dupsAllowed)
            _previousKeyString.resetFromBuffer(newKeyString.getBuffer(), newKeyString.getSize());

        return Status::OK();
    }

    Status addKeyTimestampUnsafe(const KeyString::Value& newKeyString, const RecordId& id) {
        const int cmp = newKeyString.compareWithoutRecordId(_previousKeyString);
        if (cmp != 0) {
            if (!_previousKeyString.isEmpty()) {
                // _previousKeyString.isEmpty() is only true on the first call to addKey().
                invariant(cmp > 0);  // newKey must be > the last key.
                // We are done with dups of the last key so we can insert it now.
                doInsert();
            }
            invariant(_records.empty());
        } else {
            // Dup found!
            if (!_dupsAllowed) {
                auto newKey = KeyString::toBson(newKeyString, _idx->_ordering);
                return buildDupKeyErrorStatus(
                    newKey, _idx->collectionNamespace(), _idx->indexName(), _idx->keyPattern());
            }

            // If we get here, we are in the weird mode where dups are allowed on a unique
            // index, so add ourselves to the list of duplicate ids. This also replaces the
            // _previousKey which is correct since any dups seen later are likely to be newer.
        }

        _records.push_back(std::make_pair(id, newKeyString.getTypeBits()));
        _previousKeyString.resetFromBuffer(newKeyString.getBuffer(), newKeyString.getSize());

        return Status::OK();
    }

    void doInsert() {
        invariant(!_records.empty());

        KeyString::Builder value(_idx->getKeyStringVersion());
        for (size_t i = 0; i < _records.size(); i++) {
            value.appendRecordId(_records[i].first);
            // When there is only one record, we can omit AllZeros TypeBits. Otherwise they need
            // to be included.
            if (!(_records[i].second.isAllZeros() && _records.size() == 1)) {
                value.appendTypeBits(_records[i].second);
            }
        }

        auto sizeWithoutRecordId = KeyString::sizeWithoutRecordIdAtEnd(
            _previousKeyString.getBuffer(), _previousKeyString.getSize());
        WiredTigerItem keyItem(_previousKeyString.getBuffer(), sizeWithoutRecordId);
        WiredTigerItem valueItem(value.getBuffer(), value.getSize());

        setKey(_cursor, keyItem.Get());
        _cursor->set_value(_cursor, valueItem.Get());

        invariantWTOK(_cursor->insert(_cursor));

        _records.clear();
    }

    WiredTigerIndex* _idx;
    const bool _dupsAllowed;
    KeyString::Builder _previousKeyString;
    std::vector<std::pair<RecordId, KeyString::TypeBits>> _records;
};

namespace {

/**
 * Implements the basic WT_CURSOR functionality used by both unique and standard indexes.
 */
class WiredTigerIndexCursorBase : public SortedDataInterface::Cursor {
public:
    WiredTigerIndexCursorBase(const WiredTigerIndex& idx,
                              OperationContext* opCtx,
                              bool forward,
                              KVPrefix prefix)
        : _opCtx(opCtx),
          _idx(idx),
          _forward(forward),
          _key(idx.getKeyStringVersion()),
          _typeBits(idx.getKeyStringVersion()),
          _query(idx.getKeyStringVersion()),
          _prefix(prefix) {
        _cursor.emplace(_idx.uri(), _idx.tableId(), false, _opCtx);
    }

    boost::optional<IndexKeyEntry> next(RequestedInfo parts) override {
        // Advance on a cursor at the end is a no-op
        if (_eof)
            return {};

        if (!_lastMoveSkippedKey)
            advanceWTCursor();
        updatePosition(true);
        return curr(parts);
    }

    void setEndPosition(const BSONObj& key, bool inclusive) override {
        TRACE_CURSOR << "setEndPosition inclusive: " << inclusive << ' ' << key;
        if (key.isEmpty()) {
            // This means scan to end of index.
            _endPosition.reset();
            return;
        }

        // NOTE: this uses the opposite rules as a normal seek because a forward scan should
        // end after the key if inclusive and before if exclusive.
        const auto discriminator = _forward == inclusive
            ? KeyString::Discriminator::kExclusiveAfter
            : KeyString::Discriminator::kExclusiveBefore;
        _endPosition = std::make_unique<KeyString::Builder>(_idx.getKeyStringVersion());
        _endPosition->resetToKey(BSONObj::stripFieldNames(key), _idx.getOrdering(), discriminator);
    }

    boost::optional<IndexKeyEntry> seek(const BSONObj& key,
                                        bool inclusive,
                                        RequestedInfo parts) override {
        dassert(_opCtx->lockState()->isReadLocked());
        const BSONObj finalKey = BSONObj::stripFieldNames(key);
        const auto discriminator = _forward == inclusive
            ? KeyString::Discriminator::kExclusiveBefore
            : KeyString::Discriminator::kExclusiveAfter;

        // By using a discriminator other than kInclusive, there is no need to distinguish
        // unique vs non-unique key formats since both start with the key.
        _query.resetToKey(finalKey, _idx.getOrdering(), discriminator);
        seekWTCursor(_query);
        updatePosition();
        return curr(parts);
    }

    boost::optional<IndexKeyEntry> seek(const IndexSeekPoint& seekPoint,
                                        RequestedInfo parts) override {
        dassert(_opCtx->lockState()->isReadLocked());
        // TODO: don't go to a bson obj then to a KeyString, go straight
        BSONObj key = IndexEntryComparison::makeQueryObject(seekPoint, _forward);

        // makeQueryObject handles the discriminator in the real exclusive cases.
        const auto discriminator = _forward ? KeyString::Discriminator::kExclusiveBefore
                                            : KeyString::Discriminator::kExclusiveAfter;
        _query.resetToKey(key, _idx.getOrdering(), discriminator);
        seekWTCursor(_query);
        updatePosition();
        return curr(parts);
    }

    void save() override {
        try {
            if (_cursor)
                _cursor->reset();
        } catch (const WriteConflictException&) {
            // Ignore since this is only called when we are about to kill our transaction
            // anyway.
        }

        // Our saved position is wherever we were when we last called updatePosition().
        // Any partially completed repositions should not effect our saved position.
    }

    void saveUnpositioned() override {
        save();
        _eof = true;
    }

    void restore() override {
        if (!_cursor) {
            _cursor.emplace(_idx.uri(), _idx.tableId(), false, _opCtx);
        }

        // Ensure an active session exists, so any restored cursors will bind to it
        invariant(WiredTigerRecoveryUnit::get(_opCtx)->getSession() == _cursor->getSession());

        if (!_eof) {
            // Standard (non-unique) indices *do* include the record id in their KeyStrings. This
            // means that restoring to the same key with a new record id will return false, and we
            // will *not* skip the key with the new record id.
            //
            // Unique indexes can have both kinds of KeyStrings, ie with or without the record id.
            // Restore for unique indexes gets handled separately in it's own implementation.
            _lastMoveSkippedKey = !seekWTCursor(_key);
            TRACE_CURSOR << "restore _lastMoveSkippedKey:" << _lastMoveSkippedKey;
        }
    }

    void detachFromOperationContext() final {
        _opCtx = nullptr;
        _cursor = boost::none;
    }

    void reattachToOperationContext(OperationContext* opCtx) final {
        _opCtx = opCtx;
        // _cursor recreated in restore() to avoid risk of WT_ROLLBACK issues.
    }

protected:
    // Called after _key has been filled in, ie a new key to be processed has been fetched.
    // Must not throw WriteConflictException, throwing a WriteConflictException will retry the
    // operation effectively skipping over this key.
    virtual void updateIdAndTypeBits() {
        _id = KeyString::decodeRecordIdAtEnd(_key.getBuffer(), _key.getSize());

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        // Can't get WT_ROLLBACK and hence won't throw an exception.
        // Don't expect WT_PREPARE_CONFLICT either.
        auto ret = c->get_value(c, &item);
        invariant(ret != WT_ROLLBACK && ret != WT_PREPARE_CONFLICT);
        invariantWTOK(ret);
        BufReader br(item.data, item.size);
        _typeBits.resetFromBuffer(&br);
    }

    void setKey(WT_CURSOR* cursor, const WT_ITEM* item) {
        if (_prefix == KVPrefix::kNotPrefixed) {
            cursor->set_key(cursor, item);
        } else {
            cursor->set_key(cursor, _prefix.repr(), item);
        }
    }

    void getKey(WT_CURSOR* cursor, WT_ITEM* key) {
        if (_prefix == KVPrefix::kNotPrefixed) {
            invariantWTOK(cursor->get_key(cursor, key));
        } else {
            int64_t prefix;
            invariantWTOK(cursor->get_key(cursor, &prefix, key));
            invariant(_prefix.repr() == prefix);
        }
    }

    bool hasWrongPrefix(WT_CURSOR* cursor) {
        if (_prefix == KVPrefix::kNotPrefixed) {
            return false;
        }

        int64_t prefix;
        WT_ITEM item;
        invariantWTOK(cursor->get_key(cursor, &prefix, &item));
        return _prefix.repr() != prefix;
    }

    boost::optional<IndexKeyEntry> curr(RequestedInfo parts) const {
        if (_eof)
            return {};

        dassert(!atOrPastEndPointAfterSeeking());
        dassert(!_id.isNull());

        BSONObj bson;
        if (TRACING_ENABLED || (parts & kWantKey)) {
            bson =
                KeyString::toBson(_key.getBuffer(), _key.getSize(), _idx.getOrdering(), _typeBits);

            TRACE_CURSOR << " returning " << bson << ' ' << _id;
        }

        return {{std::move(bson), _id}};
    }

    bool atOrPastEndPointAfterSeeking() const {
        if (_eof)
            return true;
        if (!_endPosition)
            return false;

        const int cmp = _key.compare(*_endPosition);

        // We set up _endPosition to be in between the last in-range value and the first
        // out-of-range value. In particular, it is constructed to never equal any legal index
        // key.
        dassert(cmp != 0);

        if (_forward) {
            // We may have landed after the end point.
            return cmp > 0;
        } else {
            // We may have landed before the end point.
            return cmp < 0;
        }
    }

    void advanceWTCursor() {
        WT_CURSOR* c = _cursor->get();
        int ret = wiredTigerPrepareConflictRetry(
            _opCtx, [&] { return _forward ? c->next(c) : c->prev(c); });
        if (ret == WT_NOTFOUND) {
            _cursorAtEof = true;
            return;
        }
        invariantWTOK(ret);
        if (hasWrongPrefix(c)) {
            _cursorAtEof = true;
            return;
        }

        _cursorAtEof = false;
    }

    // Seeks to query. Returns true on exact match.
    bool seekWTCursor(const KeyString::Builder& query) {
        WT_CURSOR* c = _cursor->get();

        int cmp = -1;
        const WiredTigerItem keyItem(query.getBuffer(), query.getSize());
        setKey(c, keyItem.Get());

        int ret = wiredTigerPrepareConflictRetry(_opCtx, [&] { return c->search_near(c, &cmp); });
        if (ret == WT_NOTFOUND) {
            _cursorAtEof = true;
            TRACE_CURSOR << "\t not found";
            return false;
        }
        invariantWTOK(ret);
        _cursorAtEof = false;

        TRACE_CURSOR << "\t cmp: " << cmp;

        if (cmp == 0) {
            // Found it!
            return true;
        }

        // Make sure we land on a matching key (after/before for forward/reverse).
        if (_forward ? cmp < 0 : cmp > 0) {
            advanceWTCursor();
        }

        return false;
    }

    /**
     * This must be called after moving the cursor to update our cached position. It should not
     * be called after a restore that did not restore to original state since that does not
     * logically move the cursor until the following call to next().
     */
    void updatePosition(bool inNext = false) {
        _lastMoveSkippedKey = false;
        if (_cursorAtEof) {
            _eof = true;
            _id = RecordId();
            return;
        }

        _eof = false;

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        getKey(c, &item);

        const auto isForwardNextCall = _forward && inNext && !_key.isEmpty();
        if (isForwardNextCall) {
            // Due to a bug in wired tiger (SERVER-21867) sometimes calling next
            // returns something prev.
            const int cmp =
                std::memcmp(_key.getBuffer(), item.data, std::min(_key.getSize(), item.size));
            bool nextNotIncreasing = cmp > 0 || (cmp == 0 && _key.getSize() > item.size);

            if (MONGO_FAIL_POINT(WTEmulateOutOfOrderNextIndexKey)) {
                log() << "WTIndex::updatePosition simulating next key not increasing.";
                nextNotIncreasing = true;
            }

            if (nextNotIncreasing) {
                // Our new key is less than the old key which means the next call moved to !next.
                log() << "WTIndex::updatePosition -- the new key ( "
                      << redact(toHex(item.data, item.size)) << ") is less than the previous key ("
                      << redact(_key.toString()) << "), which is a bug.";

                // Crash when test commands are enabled.
                invariant(!getTestCommandsEnabled());

                // Force a retry of the operation from our last known position by acting as-if
                // we received a WT_ROLLBACK error.
                throw WriteConflictException();
            }
        }

        // Store (a copy of) the new item data as the current key for this cursor.
        _key.resetFromBuffer(item.data, item.size);

        if (atOrPastEndPointAfterSeeking()) {
            _eof = true;
            return;
        }

        updateIdAndTypeBits();
    }

    OperationContext* _opCtx;
    boost::optional<WiredTigerCursor> _cursor;
    const WiredTigerIndex& _idx;  // not owned
    const bool _forward;

    // These are where this cursor instance is. They are not changed in the face of a failing
    // next().
    KeyString::Builder _key;
    KeyString::TypeBits _typeBits;
    RecordId _id;
    bool _eof = true;

    // This differs from _eof in that it always reflects the result of the most recent call to
    // reposition _cursor.
    bool _cursorAtEof = false;

    // Used by next to decide to return current position rather than moving. Should be reset to
    // false by any operation that moves the cursor, other than subsequent save/restore pairs.
    bool _lastMoveSkippedKey = false;

    KeyString::Builder _query;
    KVPrefix _prefix;

    std::unique_ptr<KeyString::Builder> _endPosition;
};

// The Standard Cursor doesn't need anything more than the base has.
using WiredTigerIndexStandardCursor = WiredTigerIndexCursorBase;

class WiredTigerIndexUniqueCursor final : public WiredTigerIndexCursorBase {
public:
    WiredTigerIndexUniqueCursor(const WiredTigerIndex& idx,
                                OperationContext* opCtx,
                                bool forward,
                                KVPrefix prefix)
        : WiredTigerIndexCursorBase(idx, opCtx, forward, prefix) {}

    // Called after _key has been filled in, ie a new key to be processed has been fetched.
    // Must not throw WriteConflictException, throwing a WriteConflictException will retry the
    // operation effectively skipping over this key.
    void updateIdAndTypeBits() override {
        TRACE_INDEX << "Unique Index KeyString: [" << _key.toString() << "]";

        // After a rolling upgrade an index can have keys from both timestamp unsafe (old) and
        // timestamp safe (new) unique indexes. Detect correct index key format by checking key's
        // size. Old format keys just had the index key while new format key has index key + Record
        // id. _id indexes remain at the old format. When KeyString contains just the key, the
        // RecordId is in value.
        if (_idx.isIdIndex() || !_idx.isTimestampSafeUniqueIdx()) {
            _updateIdAndTypeBitsFromValue();
            return;
        }

        auto keySize = KeyString::getKeySize(
            _key.getBuffer(), _key.getSize(), _idx.getOrdering(), _key.getTypeBits());

        if (_key.getSize() == keySize) {
            _updateIdAndTypeBitsFromValue();
        } else {
            // The RecordId is in the key at the end. This implementation is provided by the
            // base class, let us just invoke that functionality here.
            WiredTigerIndexCursorBase::updateIdAndTypeBits();
        }
    }

    void restore() override {
        // Lets begin by calling the base implementaion
        WiredTigerIndexCursorBase::restore();

        // If this is not timestamp safe unique index, we are done
        if (_idx.isIdIndex() || !_idx.isTimestampSafeUniqueIdx()) {
            return;
        }

        if (_lastMoveSkippedKey && !_eof && !_cursorAtEof) {
            // We did not get an exact match for the saved key. We need to determine if we
            // skipped a record while trying to position the cursor.
            // After a rolling upgrade an index can have keys from both timestamp unsafe (old)
            // and timestamp safe (new) unique indexes. An older styled index entry key is
            // KeyString of the prefix key only, whereas a newer styled index entry key is
            // KeyString of the prefix key + RecordId.
            // In either case we compare the prefix key portion of the saved index entry
            // key against the current key that we are positioned on, if there is a match we
            // know we are positioned correctly and have not skipped a record.
            WT_ITEM item;
            WT_CURSOR* c = _cursor->get();
            getKey(c, &item);

            // Get the size of the prefix key
            auto keySize = KeyString::getKeySize(
                _key.getBuffer(), _key.getSize(), _idx.getOrdering(), _key.getTypeBits());

            // This check is only to avoid returning the same key again after a restore. Keys
            // shorter than _key cannot have "prefix key" same as _key. Therefore we care only about
            // the keys with size greater than or equal to that of the _key.
            if (item.size >= keySize && std::memcmp(_key.getBuffer(), item.data, keySize) == 0) {
                _lastMoveSkippedKey = false;
                TRACE_CURSOR << "restore _lastMoveSkippedKey changed to false.";
            }
        }
    }

private:
    // Called after _key has been filled in, ie a new key to be processed has been fetched.
    // Must not throw WriteConflictException, throwing a WriteConflictException will retry the
    // operation effectively skipping over this key.
    void _updateIdAndTypeBitsFromValue() {
        // We assume that cursors can only ever see unique indexes in their "pristine" state,
        // where no duplicates are possible. The cases where dups are allowed should hold
        // sufficient locks to ensure that no cursor ever sees them.
        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        // Can't get WT_ROLLBACK and hence won't throw an exception.
        // Don't expect WT_PREPARE_CONFLICT either.
        auto ret = c->get_value(c, &item);
        invariant(ret != WT_ROLLBACK && ret != WT_PREPARE_CONFLICT);
        invariantWTOK(ret);

        BufReader br(item.data, item.size);
        _id = KeyString::decodeRecordId(&br);
        _typeBits.resetFromBuffer(&br);

        if (!br.atEof()) {
            severe() << "Unique index cursor seeing multiple records for key "
                     << redact(curr(kWantKey)->key) << " in index " << _idx.indexName() << " ("
                     << _idx.uri() << ") belonging to collection " << _idx.collectionNamespace();
            fassertFailed(28608);
        }
    }
};

}  // namespace

WiredTigerIndexUnique::WiredTigerIndexUnique(OperationContext* ctx,
                                             const std::string& uri,
                                             const IndexDescriptor* desc,
                                             KVPrefix prefix,
                                             bool isReadOnly)
    : WiredTigerIndex(ctx, uri, desc, prefix, isReadOnly), _partial(desc->isPartial()) {}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIndexUnique::newCursor(
    OperationContext* opCtx, bool forward) const {
    return std::make_unique<WiredTigerIndexUniqueCursor>(*this, opCtx, forward, _prefix);
}

SortedDataBuilderInterface* WiredTigerIndexUnique::getBulkBuilder(OperationContext* opCtx,
                                                                  bool dupsAllowed) {
    return new UniqueBulkBuilder(this, opCtx, dupsAllowed, _prefix);
}

bool WiredTigerIndexUnique::isTimestampSafeUniqueIdx() const {
    if (_dataFormatVersion == kDataFormatV1KeyStringV0IndexVersionV1 ||
        _dataFormatVersion == kDataFormatV2KeyStringV1IndexVersionV2) {
        return false;
    }
    return true;
}

bool WiredTigerIndexUnique::_keyExists(OperationContext* opCtx,
                                       WT_CURSOR* c,
                                       const char* buffer,
                                       size_t size) {
    WiredTigerItem prefixKeyItem(buffer, size);
    setKey(c, prefixKeyItem.Get());

    // An index entry key is KeyString of the prefix key + RecordId. To prevent duplicate prefix
    // key, search a record matching the prefix key.
    int cmp;
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search_near(c, &cmp); });

    if (ret == WT_NOTFOUND)
        return false;
    invariantWTOK(ret);

    if (cmp == 0)
        return true;

    WT_ITEM item;
    // Obtain the key from the record returned by search near.
    getKey(c, &item);
    if (std::memcmp(buffer, item.data, std::min(size, item.size)) == 0) {
        return true;
    }

    // If the prefix does not match, look at the logically adjacent key.
    if (cmp < 0) {
        // We got the smaller key adjacent to prefix key, check the next key too.
        ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->next(c); });
    } else {
        // We got the larger key adjacent to prefix key, check the previous key too.
        ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->prev(c); });
    }

    if (ret == WT_NOTFOUND) {
        return false;
    }
    invariantWTOK(ret);

    getKey(c, &item);
    return std::memcmp(buffer, item.data, std::min(size, item.size)) == 0;
}

bool WiredTigerIndexUnique::isDup(OperationContext* opCtx, WT_CURSOR* c, const BSONObj& key) {
    if (!isTimestampSafeUniqueIdx()) {
        // The parent class provides a functionality that works fine, just use that.
        return WiredTigerIndex::isDup(opCtx, c, key);
    }

    // This procedure to determine duplicates is exclusive for timestamp safe unique indexes.
    KeyString::Builder prefixKey(getKeyStringVersion(), key, _ordering);
    // Check if a prefix key already exists in the index. When keyExists() returns true, the cursor
    // will be positioned on the first occurence of the 'prefixKey'.
    if (!_keyExists(opCtx, c, prefixKey.getBuffer(), prefixKey.getSize())) {
        return false;
    }

    // If the next key also matches, this is a duplicate.
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->next(c); });

    WT_ITEM item;
    if (ret == 0) {
        getKey(c, &item);
        return std::memcmp(
                   prefixKey.getBuffer(), item.data, std::min(prefixKey.getSize(), item.size)) == 0;
    }

    // Make sure that next call did not fail due to any other error but not found. In case of
    // another error, we are not good to move forward.
    if (ret == WT_NOTFOUND) {
        return false;
    }

    fassertFailedWithStatus(40685, wtRCToStatus(ret));
    MONGO_UNREACHABLE;
}

Status WiredTigerIndexUnique::_insert(OperationContext* opCtx,
                                      WT_CURSOR* c,
                                      const KeyString::Value& keyString,
                                      const RecordId& id,
                                      bool dupsAllowed) {
    if (isTimestampSafeUniqueIdx()) {
        return _insertTimestampSafe(opCtx, c, keyString, dupsAllowed);
    }
    return _insertTimestampUnsafe(opCtx, c, keyString, id, dupsAllowed);
}

Status WiredTigerIndexUnique::_insertTimestampUnsafe(OperationContext* opCtx,
                                                     WT_CURSOR* c,
                                                     const KeyString::Value& keyString,
                                                     const RecordId& id,
                                                     bool dupsAllowed) {
    invariant(id.isValid());

    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);

    KeyString::Builder value(getKeyStringVersion(), id);
    if (!keyString.getTypeBits().isAllZeros())
        value.appendTypeBits(keyString.getTypeBits());

    WiredTigerItem valueItem(value.getBuffer(), value.getSize());
    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    int ret = WT_OP_CHECK(c->insert(c));

    if (ret != WT_DUPLICATE_KEY) {
        if (ret == 0) {
            return Status::OK();
        }

        return wtRCToStatus(ret);
    }

    // we might be in weird mode where there might be multiple values
    // we put them all in the "list"
    // Note that we can't omit AllZeros when there are multiple ids for a value. When we remove
    // down to a single value, it will be cleaned up.
    ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
    invariantWTOK(ret);

    WT_ITEM old;
    invariantWTOK(c->get_value(c, &old));

    bool insertedId = false;

    value.resetToEmpty();
    BufReader br(old.data, old.size);
    while (br.remaining()) {
        RecordId idInIndex = KeyString::decodeRecordId(&br);
        if (id == idInIndex)
            return Status::OK();

        if (!insertedId && id < idInIndex) {
            value.appendRecordId(id);
            value.appendTypeBits(keyString.getTypeBits());
            insertedId = true;
        }

        // Copy from old to new value
        value.appendRecordId(idInIndex);
        value.appendTypeBits(KeyString::TypeBits::fromBuffer(getKeyStringVersion(), &br));
    }

    if (!dupsAllowed) {
        auto key = KeyString::toBson(keyString, _ordering);
        return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
    }

    if (!insertedId) {
        // This id is higher than all currently in the index for this key
        value.appendRecordId(id);
        value.appendTypeBits(keyString.getTypeBits());
    }

    valueItem = WiredTigerItem(value.getBuffer(), value.getSize());
    c->set_value(c, valueItem.Get());
    Status status = wtRCToStatus(c->update(c));

    if (!status.isOK())
        return status;

    return Status::OK();
}

Status WiredTigerIndexUnique::_insertTimestampSafe(OperationContext* opCtx,
                                                   WT_CURSOR* c,
                                                   const KeyString::Value& keyString,
                                                   bool dupsAllowed) {
    TRACE_INDEX << "Timestamp safe unique idx KeyString: " << keyString;

    int ret;

    // Pre-checks before inserting on a primary.
    if (!dupsAllowed) {
        // A prefix key is KeyString of index key. It is the component of the index entry that
        // should be unique.
        auto sizeWithoutRecordId =
            KeyString::sizeWithoutRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());
        WiredTigerItem prefixKeyItem(keyString.getBuffer(), sizeWithoutRecordId);

        // First phase inserts the prefix key to prohibit concurrent insertions of same key
        setKey(c, prefixKeyItem.Get());
        c->set_value(c, emptyItem.Get());
        ret = WT_OP_CHECK(c->insert(c));

        // An entry with prefix key already exists. This can happen only during rolling upgrade when
        // both timestamp unsafe and timestamp safe index format keys could be present.
        if (ret == WT_DUPLICATE_KEY) {
            auto key = KeyString::toBson(
                keyString.getBuffer(), sizeWithoutRecordId, _ordering, keyString.getTypeBits());
            return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
        }
        invariantWTOK(ret);

        // Remove the prefix key, our entry will continue to conflict with any concurrent
        // transactions, but will not conflict with any transaction that begins after this
        // operation commits.
        setKey(c, prefixKeyItem.Get());
        ret = WT_OP_CHECK(c->remove(c));
        invariantWTOK(ret);

        // Second phase looks up for existence of key to avoid insertion of duplicate key
        if (_keyExists(opCtx, c, keyString.getBuffer(), sizeWithoutRecordId)) {
            auto key = KeyString::toBson(
                keyString.getBuffer(), sizeWithoutRecordId, _ordering, keyString.getTypeBits());
            return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
        }
    }

    // Now create the table key/value, the actual data record.
    WiredTigerItem keyItem(keyString.getBuffer(), keyString.getSize());

    WiredTigerItem valueItem = keyString.getTypeBits().isAllZeros()
        ? emptyItem
        : WiredTigerItem(keyString.getTypeBits().getBuffer(), keyString.getTypeBits().getSize());
    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    ret = WT_OP_CHECK(c->insert(c));

    // It is possible that this key is already present during a concurrent background index build.
    if (ret != WT_DUPLICATE_KEY)
        invariantWTOK(ret);

    return Status::OK();
}

void WiredTigerIndexUnique::_unindex(OperationContext* opCtx,
                                     WT_CURSOR* c,
                                     const KeyString::Value& keyString,
                                     const RecordId& id,
                                     bool dupsAllowed) {
    if (isTimestampSafeUniqueIdx()) {
        return _unindexTimestampSafe(opCtx, c, keyString, dupsAllowed);
    }
    return _unindexTimestampUnsafe(opCtx, c, keyString, id, dupsAllowed);
}

void WiredTigerIndexUnique::_unindexTimestampUnsafe(OperationContext* opCtx,
                                                    WT_CURSOR* c,
                                                    const KeyString::Value& keyString,
                                                    const RecordId& id,
                                                    bool dupsAllowed) {
    invariant(id.isValid());

    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);
    setKey(c, keyItem.Get());

    auto triggerWriteConflictAtPoint = [this, &keyItem](WT_CURSOR* point) {
        // WT_NOTFOUND may occur during a background index build. Insert a dummy value and
        // delete it again to trigger a write conflict in case this is being concurrently
        // indexed by the background indexer.
        setKey(point, keyItem.Get());
        point->set_value(point, emptyItem.Get());
        invariantWTOK(WT_OP_CHECK(point->insert(point)));
        setKey(point, keyItem.Get());
        invariantWTOK(WT_OP_CHECK(point->remove(point)));
    };

    if (!dupsAllowed) {
        if (_partial) {
            // Check that the record id matches.  We may be called to unindex records that are not
            // present in the index due to the partial filter expression.
            int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
            if (ret == WT_NOTFOUND) {
                triggerWriteConflictAtPoint(c);
                return;
            }
            invariantWTOK(ret);
            WT_ITEM value;
            invariantWTOK(c->get_value(c, &value));
            BufReader br(value.data, value.size);
            fassert(40416, br.remaining());
            if (KeyString::decodeRecordId(&br) != id) {
                return;
            }
            // Ensure there aren't any other values in here.
            KeyString::TypeBits::fromBuffer(getKeyStringVersion(), &br);
            fassert(40417, !br.remaining());
        }
        int ret = WT_OP_CHECK(c->remove(c));
        if (ret == WT_NOTFOUND) {
            triggerWriteConflictAtPoint(c);
            return;
        }
        invariantWTOK(ret);
        return;
    }

    // dups are allowed, so we have to deal with a vector of RecordIds.

    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
    if (ret == WT_NOTFOUND) {
        triggerWriteConflictAtPoint(c);
        return;
    }
    invariantWTOK(ret);

    WT_ITEM old;
    invariantWTOK(c->get_value(c, &old));

    bool foundId = false;
    std::vector<std::pair<RecordId, KeyString::TypeBits>> records;

    BufReader br(old.data, old.size);
    while (br.remaining()) {
        RecordId idInIndex = KeyString::decodeRecordId(&br);
        KeyString::TypeBits typeBits = KeyString::TypeBits::fromBuffer(getKeyStringVersion(), &br);

        if (id == idInIndex) {
            if (records.empty() && !br.remaining()) {
                // This is the common case: we are removing the only id for this key.
                // Remove the whole entry.
                invariantWTOK(WT_OP_CHECK(c->remove(c)));
                return;
            }

            foundId = true;
            continue;
        }

        records.push_back(std::make_pair(idInIndex, typeBits));
    }

    if (!foundId) {
        auto key = KeyString::toBson(keyString, _ordering);
        warning().stream() << id << " not found in the index for key " << redact(key);
        return;  // nothing to do
    }

    // Put other ids for this key back in the index.
    KeyString::Builder newValue(getKeyStringVersion());
    invariant(!records.empty());
    for (size_t i = 0; i < records.size(); i++) {
        newValue.appendRecordId(records[i].first);
        // When there is only one record, we can omit AllZeros TypeBits. Otherwise they need
        // to be included.
        if (!(records[i].second.isAllZeros() && records.size() == 1)) {
            newValue.appendTypeBits(records[i].second);
        }
    }

    WiredTigerItem valueItem = WiredTigerItem(newValue.getBuffer(), newValue.getSize());
    c->set_value(c, valueItem.Get());
    invariantWTOK(c->update(c));
}

void WiredTigerIndexUnique::_unindexTimestampSafe(OperationContext* opCtx,
                                                  WT_CURSOR* c,
                                                  const KeyString::Value& keyString,
                                                  bool dupsAllowed) {
    WiredTigerItem item(keyString.getBuffer(), keyString.getSize());
    setKey(c, item.Get());
    int ret = WT_OP_CHECK(c->remove(c));
    if (ret != WT_NOTFOUND) {
        invariantWTOK(ret);
        return;
    }

    // After a rolling upgrade an index can have keys from both timestamp unsafe (old) and
    // timestamp safe (new) unique indexes. Old format keys just had the index key while new
    // format key has index key + Record id. WT_NOTFOUND is possible if index key is in old format.
    // Retry removal of key using old format.
    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);
    setKey(c, keyItem.Get());

    ret = WT_OP_CHECK(c->remove(c));
    if (ret != WT_NOTFOUND) {
        invariantWTOK(ret);
        return;
    }
    // Otherwise WT_NOTFOUND is only expected during a background index build. Insert a dummy value
    // and delete it again to trigger a write conflict in case this is being concurrently indexed
    // by the background indexer.
    setKey(c, item.Get());
    c->set_value(c, emptyItem.Get());
    invariantWTOK(WT_OP_CHECK(c->insert(c)));
    setKey(c, item.Get());
    invariantWTOK(WT_OP_CHECK(c->remove(c)));
}
// ------------------------------

WiredTigerIndexStandard::WiredTigerIndexStandard(OperationContext* ctx,
                                                 const std::string& uri,
                                                 const IndexDescriptor* desc,
                                                 KVPrefix prefix,
                                                 bool isReadOnly)
    : WiredTigerIndex(ctx, uri, desc, prefix, isReadOnly) {}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIndexStandard::newCursor(
    OperationContext* opCtx, bool forward) const {
    return std::make_unique<WiredTigerIndexStandardCursor>(*this, opCtx, forward, _prefix);
}

SortedDataBuilderInterface* WiredTigerIndexStandard::getBulkBuilder(OperationContext* opCtx,
                                                                    bool dupsAllowed) {
    // We aren't unique so dups better be allowed.
    invariant(dupsAllowed);
    return new StandardBulkBuilder(this, opCtx, _prefix);
}

Status WiredTigerIndexStandard::_insert(OperationContext* opCtx,
                                        WT_CURSOR* c,
                                        const KeyString::Value& keyString,
                                        const RecordId& id,
                                        bool dupsAllowed) {
    invariant(dupsAllowed);

    WiredTigerItem keyItem(keyString.getBuffer(), keyString.getSize());

    WiredTigerItem valueItem = keyString.getTypeBits().isAllZeros()
        ? emptyItem
        : WiredTigerItem(keyString.getTypeBits().getBuffer(), keyString.getTypeBits().getSize());

    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    int ret = WT_OP_CHECK(c->insert(c));

    // If the record was already in the index, we just return OK.
    // This can happen, for example, when building a background index while documents are being
    // written and reindexed.
    if (ret != 0 && ret != WT_DUPLICATE_KEY)
        return wtRCToStatus(ret);

    return Status::OK();
}

void WiredTigerIndexStandard::_unindex(OperationContext* opCtx,
                                       WT_CURSOR* c,
                                       const KeyString::Value& keyString,
                                       const RecordId&,
                                       bool dupsAllowed) {
    invariant(dupsAllowed);
    WiredTigerItem item(keyString.getBuffer(), keyString.getSize());
    setKey(c, item.Get());
    int ret = WT_OP_CHECK(c->remove(c));
    if (ret != WT_NOTFOUND) {
        invariantWTOK(ret);
    } else {
        // WT_NOTFOUND is only expected during a background index build. Insert a dummy value and
        // delete it again to trigger a write conflict in case this is being concurrently indexed by
        // the background indexer.
        setKey(c, item.Get());
        c->set_value(c, emptyItem.Get());
        invariantWTOK(WT_OP_CHECK(c->insert(c)));
        setKey(c, item.Get());
        invariantWTOK(WT_OP_CHECK(c->remove(c)));
    }
}

}  // namespace mongo
