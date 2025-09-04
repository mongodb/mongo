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

#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)

#include <wiredtiger.h>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/spill_wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_compiled_configuration.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <algorithm>
#include <cstring>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {

struct RecordIdAndWall {
    RecordId id;
    Date_t wall;

    RecordIdAndWall(RecordId lastRecord, Date_t wallTime)
        : id(std::move(lastRecord)), wall(wallTime) {}
};

WiredTigerRecordStore::CursorKey makeCursorKey(const RecordId& rid, KeyFormat format) {
    if (format == KeyFormat::Long) {
        return rid.getLong();
    } else {
        auto str = rid.getStr();
        return WiredTigerItem(str.data(), str.size());
    }
}

static const int kMinimumRecordStoreVersion = 1;
static const int kCurrentRecordStoreVersion = 1;  // New record stores use this by default.
static const int kMaximumRecordStoreVersion = 1;
MONGO_STATIC_ASSERT(kCurrentRecordStoreVersion >= kMinimumRecordStoreVersion);
MONGO_STATIC_ASSERT(kCurrentRecordStoreVersion <= kMaximumRecordStoreVersion);

static CompiledConfiguration lowerInclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=lower,inclusive=true");
static CompiledConfiguration lowerExclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=lower,inclusive=false");
static CompiledConfiguration upperInclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=upper,inclusive=true");
static CompiledConfiguration upperExclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=upper,inclusive=false");
static CompiledConfiguration clearBoundConfig("WT_CURSOR.bound", "action=clear");

void checkOplogFormatVersion(WiredTigerRecoveryUnit& ru, const std::string& uri) {
    StatusWith<BSONObj> appMetadata =
        WiredTigerUtil::getApplicationMetadata(*ru.getSessionNoTxn(), uri);
    fassert(39999, appMetadata);

    fassertNoTrace(39998, appMetadata.getValue().getIntField("oplogKeyExtractionVersion") == 1);
}

void appendNumericStats(WiredTigerSession& s, const std::string& uri, BSONObjBuilder& bob) {
    Status status =
        WiredTigerUtil::exportTableToBSON(s, "statistics:" + uri, "statistics=(fast)", bob);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }
}

std::size_t computeRecordIdSize(const RecordId& id) {
    // We previously weren't accounting for WiredTiger key size when it was an int64_t, thus we
    // return 0 in those cases. With the clustering capabilities we now support potentially large
    // keys as they are byte arrays, thus having to take it into account for the read/write metrics.
    return id.isStr() ? id.getStr().size() : 0;
}

RecordId getKey(WT_CURSOR* cursor, KeyFormat keyFormat) {
    if (keyFormat == KeyFormat::String) {
        WiredTigerItem item;
        invariantWTOK(cursor->get_key(cursor, &item), cursor->session);
        return RecordId(item);
    } else {
        std::int64_t recordId;
        invariantWTOK(cursor->get_key(cursor, &recordId), cursor->session);
        return RecordId(recordId);
    }
}

RecordData getRecordData(WT_CURSOR* c) {
    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value), c->session);
    return {static_cast<const char*>(value.data), static_cast<int>(value.size)};
}


void setKey(WT_CURSOR* cursor, const WiredTigerRecordStore::CursorKey* key) {
    if (auto itemPtr = get_if<WiredTigerItem>(key)) {
        cursor->set_key(cursor, itemPtr->get());
    } else if (auto longPtr = get_if<int64_t>(key)) {
        cursor->set_key(cursor, *longPtr);
    }
}
}  // namespace

MONGO_FAIL_POINT_DEFINE(WTCompactRecordStoreEBUSY);
MONGO_FAIL_POINT_DEFINE(WTRecordStoreUassertOutOfOrder);
MONGO_FAIL_POINT_DEFINE(WTWriteConflictException);
MONGO_FAIL_POINT_DEFINE(WTWriteConflictExceptionForReads);

std::variant<WiredTigerIntegerKeyedContainer, WiredTigerStringKeyedContainer>
WiredTigerRecordStore::_makeContainer(Params& params) {
    switch (params.keyFormat) {
        case KeyFormat::Long: {
            auto container =
                WiredTigerIntegerKeyedContainer(std::make_shared<Ident>(params.ident),
                                                WiredTigerUtil::kTableUriPrefix + params.ident,
                                                WiredTigerUtil::genTableId());
            return container;
        }
        case KeyFormat::String: {
            auto container =
                WiredTigerStringKeyedContainer(std::make_shared<Ident>(params.ident),
                                               WiredTigerUtil::kTableUriPrefix + params.ident,
                                               WiredTigerUtil::genTableId());
            return container;
        }
    }
    MONGO_UNREACHABLE;
};

StatusWith<std::string> WiredTigerRecordStore::parseOptionsField(const BSONObj options) {
    StringBuilder ss;
    for (auto&& elem : options) {
        if (elem.fieldNameStringData() == WiredTigerUtil::kConfigStringField) {
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

std::string WiredTigerRecordStore::generateCreateString(
    StringData tableName,
    const WiredTigerRecordStore::WiredTigerTableConfig& wtTableConfig,
    bool isOplog) {

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

    ss << "block_compressor=" << wtTableConfig.blockCompressor << ",";

    // TODO: Replace WiredTigerCustomizationHooks with WiredTigerCustomizationHooksRegistry.
    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig(tableName);
    ss << WiredTigerCustomizationHooksRegistry::get(getGlobalServiceContext())
              .getTableCreateConfig(tableName);

    ss << wtTableConfig.extraCreateOptions << ",";

    // By default, WiredTiger silently ignores a create table command if the specified ident already
    // exists - even if the existing table has a different configuration.
    //
    // Enable the 'exclusive' flag so WiredTiger table creation fails if an ident already exists.
    ss << "exclusive=true,";

    // WARNING: No user-specified config can appear below this line. These options are required
    // for correct behavior of the server.
    if (wtTableConfig.keyFormat == KeyFormat::String) {
        // If the RecordId format is a String, assume a byte array key format.
        ss << "key_format=u";
    } else {
        // All other collections use an int64_t as their table keys.
        ss << "key_format=q";
    }

    // Record store metadata
    ss << ",app_metadata=(formatVersion=" << kCurrentRecordStoreVersion;
    if (isOplog) {
        ss << ",oplogKeyExtractionVersion=1";
    }
    ss << ")";

    if (wtTableConfig.logEnabled) {
        ss << ",log=(enabled=true)";
    } else {
        ss << ",log=(enabled=false)";
    }

    return ss;
}

void WiredTigerRecordStore::wtDeleteRecord(OperationContext* opCtx,
                                           WiredTigerRecoveryUnit& wtRu,
                                           const RecordId& id,
                                           OpStats& opStats) {
    opStats = OpStats{};
    opStats.keyLength = computeRecordIdSize(id);

    auto cursorParams = getWiredTigerCursorParams(wtRu, tableId(), /*allowOverwrite=*/true);
    WiredTigerCursor cursor(std::move(cursorParams), getURI(), *wtRu.getSession());
    WT_CURSOR* c = cursor.get();
    CursorKey key = makeCursorKey(id, keyFormat());
    setKey(c, &key);
    int ret = wiredTigerPrepareConflictRetry(
        *opCtx, StorageExecutionContext::get(opCtx)->getPrepareConflictTracker(), wtRu, [&] {
            return c->search(c);
        });
    if (ret == WT_NOTFOUND) {
        if (TestingProctor::instance().isEnabled()) {
            LOGV2_FATAL(9099700,
                        "Record to be deleted not found",
                        "uuid"_attr = uuid(),
                        "RecordId"_attr = id);
        } else {
            // Return early without crash if in production.
            LOGV2_ERROR(9099701,
                        "Record to be deleted not found",
                        "uuid"_attr = uuid(),
                        "RecordId"_attr = id);
            printStackTrace();
            return;
        }
    }
    invariantWTOK(ret, c->session);

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret, c->session);

    opStats.oldValueLength = old_value.size;

    ret = WT_OP_CHECK(wiredTigerCursorRemove(wtRu, c));
    invariantWTOK(ret, c->session);
}

Status WiredTigerRecordStore::wtInsertRecord(OperationContext* opCtx,
                                             WiredTigerRecoveryUnit& wtRu,
                                             WT_CURSOR* c,
                                             const Record& record,
                                             OpStats& opStats) {
    opStats = OpStats{};

    CursorKey key = makeCursorKey(record.id, keyFormat());
    setKey(c, &key);
    WiredTigerItem value(record.data.data(), record.data.size());
    c->set_value(c, value.get());
    int ret = WT_OP_CHECK(wiredTigerCursorInsert(wtRu, c));

    if (ret == WT_DUPLICATE_KEY) {
        invariant(!_overwrite);
        invariant(keyFormat() == KeyFormat::String);

        BSONObj foundValueObj;
        if (TestingProctor::instance().isEnabled()) {
            WT_ITEM foundValue;
            invariantWTOK(c->get_value(c, &foundValue), c->session);
            foundValueObj = BSONObj(reinterpret_cast<const char*>(foundValue.data));
        }

        return Status{
            DuplicateKeyErrorInfo{
                BSONObj(), BSONObj(), BSONObj(), std::move(foundValueObj), std::move(record.id)},
            "Duplicate cluster key found"};
    }

    if (ret)
        return wtRCToStatus(ret, c->session, "WiredTigerRecordStore::insertRecord");

    opStats.keyLength = computeRecordIdSize(record.id);
    opStats.newValueLength = value.size();

    return Status::OK();
}

Status WiredTigerRecordStore::wtUpdateRecord(OperationContext* opCtx,
                                             WiredTigerRecoveryUnit& wtRu,
                                             const RecordId& id,
                                             const char* data,
                                             int len,
                                             OpStats& opStats) {
    opStats = OpStats{};

    auto cursorParams = getWiredTigerCursorParams(wtRu, tableId(), /*allowOverwrite=*/true);
    WiredTigerCursor curwrap(std::move(cursorParams), getURI(), *wtRu.getSession());
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    auto key = makeCursorKey(id, keyFormat());
    setKey(c, &key);
    int ret = wiredTigerPrepareConflictRetry(
        *opCtx, StorageExecutionContext::get(opCtx)->getPrepareConflictTracker(), wtRu, [&] {
            return c->search(c);
        });

    invariantWTOK(
        ret,
        c->session,
        str::stream() << "UUID: " << (uuid() ? uuid()->toString() : std::string{})
                      << "; Key: " << getKey(c, keyFormat()) << "; Read Timestamp: "
                      << wtRu.getPointInTimeReadTimestamp().value_or(Timestamp{}).toString());

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret, c->session);

    opStats.keyLength = computeRecordIdSize(id);
    opStats.oldValueLength = old_value.size;
    opStats.newValueLength = len;

    WiredTigerItem value(data, len);

    // Check if we should modify rather than doing a full update.  Look for deltas for documents
    // larger than 1KB, up to 16 changes representing up to 10% of the data.
    //
    // Skip modify for logged tables: don't trust WiredTiger's recovery with operations that are not
    // idempotent.
    const int kMinLengthForDiff = 1024;
    const int kMaxEntries = 16;
    const int kMaxDiffBytes = len / 10;

    bool skip_update = false;
    if (!_forceUpdateWithFullDocument && !_isLogged && len > kMinLengthForDiff &&
        len <= opStats.oldValueLength + kMaxDiffBytes) {
        int nentries = kMaxEntries;
        std::vector<WT_MODIFY> entries(nentries);

        if ((ret = wiredtiger_calc_modify(
                 c->session, &old_value, value.get(), kMaxDiffBytes, entries.data(), &nentries)) ==
            0) {
            invariantWTOK(WT_OP_CHECK(nentries == 0 ? c->reserve(c)
                                                    : wiredTigerCursorModify(
                                                          wtRu, c, entries.data(), nentries)),
                          c->session);

            WT_ITEM new_value;
            dassert(nentries == 0 ||
                    (c->get_value(c, &new_value) == 0 && new_value.size == value.size() &&
                     memcmp(data, new_value.data, len) == 0));
            skip_update = true;
        } else if (ret != WT_NOTFOUND) {
            invariantWTOK(ret, c->session);
        }
    }

    if (!skip_update) {
        c->set_value(c, value.get());
        ret = WT_OP_CHECK(wiredTigerCursorInsert(wtRu, c));
    }
    invariantWTOK(ret, c->session);

    return Status::OK();
}

void WiredTigerRecordStore::wtTruncate(OperationContext* opCtx, WiredTigerRecoveryUnit& wtRu) {
    WiredTigerUtil::truncate(wtRu, getURI());
}

void WiredTigerRecordStore::wtRangeTruncate(WiredTigerRecoveryUnit& wtRu,
                                            const RecordId& minRecordId,
                                            const RecordId& maxRecordId) {
    auto cursorParams = getWiredTigerCursorParams(wtRu, tableId(), /*allowOverwrite=*/true);

    WiredTigerCursor startWrap(cursorParams, getURI(), *wtRu.getSession());
    boost::optional<CursorKey> startKey;
    WT_CURSOR* start = [&]() -> WT_CURSOR* {
        if (minRecordId == RecordId()) {
            return nullptr;
        }
        startKey = makeCursorKey(minRecordId, keyFormat());
        setKey(startWrap.get(), &(*startKey));
        return startWrap.get();
    }();

    WiredTigerCursor endWrap(std::move(cursorParams), getURI(), *wtRu.getSession());
    boost::optional<CursorKey> endKey;
    WT_CURSOR* finish = [&]() -> WT_CURSOR* {
        if (maxRecordId == RecordId()) {
            return nullptr;
        }
        endKey = makeCursorKey(maxRecordId, keyFormat());
        setKey(endWrap.get(), &(*endKey));
        return endWrap.get();
    }();

    WiredTigerSession* session = wtRu.getSession();
    invariantWTOK(WT_OP_CHECK(session->truncate(nullptr, start, finish, nullptr)), *session);
}

StatusWith<int64_t> WiredTigerRecordStore::wtCompact(OperationContext* opCtx,
                                                     WiredTigerRecoveryUnit& wtRu,
                                                     const CompactOptions& options) {
    WiredTigerConnection* connection = wtRu.getConnection();
    if (connection->isEphemeral()) {
        return 0;
    }

    WiredTigerSession* s = wtRu.getSession();
    wtRu.abandonSnapshot();

    StringBuilder config;
    config << "timeout=0";
    if (options.dryRun) {
        config << ",dryrun=true";
    }
    if (options.freeSpaceTargetMB) {
        config << ",free_space_target=" << std::to_string(*options.freeSpaceTargetMB) << "MB";
    }
    const std::string uri(getURI());
    int ret = s->compact(uri.c_str(), config.str().c_str());

    if (ret == WT_ERROR && !opCtx->checkForInterruptNoAssert().isOK()) {
        return Status(ErrorCodes::Interrupted,
                      str::stream() << "Storage compaction interrupted on " << uri);
    }

    if (MONGO_unlikely(WTCompactRecordStoreEBUSY.shouldFail())) {
        ret = EBUSY;
    }

    if (ret == EBUSY) {
        return Status(ErrorCodes::Interrupted,
                      str::stream() << "Compaction interrupted on " << getURI());
    }
    invariantWTOK(ret, *s);

    return options.dryRun ? WiredTigerUtil::getIdentCompactRewrittenExpectedSize(*s, uri) : 0;
}

class WiredTigerRecordStore::RandomCursor final : public RecordCursor {
public:
    RandomCursor(OperationContext* opCtx, RecoveryUnit& ru, const WiredTigerRecordStore& rs)
        : _cursor(nullptr),
          _keyFormat(rs.keyFormat()),
          _uri(rs.getURI()),
          _opCtx(opCtx),
          _ru(&ru),
          _tableId(rs.tableId()) {
        restore(ru);
    }

    ~RandomCursor() override = default;

    boost::optional<Record> next() final {
        int advanceRet = wiredTigerPrepareConflictRetry(
            *_opCtx, StorageExecutionContext::get(_opCtx)->getPrepareConflictTracker(), *_ru, [&] {
                return _cursor->get()->next(_cursor->get());
            });
        if (advanceRet == WT_NOTFOUND)
            return {};
        invariantWTOK(advanceRet, *_cursor->getSession());

        RecordId id;
        if (_keyFormat == KeyFormat::String) {
            WiredTigerItem item;
            invariantWTOK(_cursor->get()->get_key(_cursor->get(), &item), *_cursor->getSession());
            id = RecordId(item);
        } else {
            int64_t key;
            invariantWTOK(_cursor->get()->get_key(_cursor->get(), &key), *_cursor->getSession());
            id = RecordId(key);
        }

        WT_ITEM value;
        invariantWTOK(_cursor->get()->get_value(_cursor->get(), &value), *_cursor->getSession());

        return {
            {std::move(id), {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
    }

    void save() final {
        if (_cursor) {
            invariantWTOK(WT_READ_CHECK(_cursor->get()->reset(_cursor->get())),
                          *_cursor->getSession());
        }
        _ru = nullptr;
    }

    bool restore(RecoveryUnit& ru, bool tolerateCappedRepositioning = true) final {
        _ru = &ru;
        auto& wtRu = WiredTigerRecoveryUnit::get(*_ru);

        if (!_cursor) {
            auto cursorParams = getWiredTigerCursorParams(
                wtRu, _tableId, false /* allowOverwrite */, true /* random */);
            _cursor = std::make_unique<WiredTigerCursor>(
                std::move(cursorParams), _uri, *wtRu.getSession());
        }
        return true;
    }

    void detachFromOperationContext() final {
        invariant(_opCtx);
        _opCtx = nullptr;
        if (!_saveStorageCursorOnDetachFromOperationContext) {
            _cursor.reset();
        }
    }

    void reattachToOperationContext(OperationContext* opCtx) final {
        invariant(!_opCtx);
        _opCtx = opCtx;
    }

    void setSaveStorageCursorOnDetachFromOperationContext(bool saveCursor) override {
        _saveStorageCursorOnDetachFromOperationContext = saveCursor;
    }

private:
    std::unique_ptr<WiredTigerCursor> _cursor;
    KeyFormat _keyFormat;
    const std::string _uri;
    OperationContext* _opCtx;
    RecoveryUnit* _ru;
    const uint64_t _tableId;
    bool _saveStorageCursorOnDetachFromOperationContext = false;
};

WiredTigerRecordStore::WiredTigerRecordStore(WiredTigerKVEngineBase* kvEngine,
                                             WiredTigerRecoveryUnit& ru,
                                             Params params)
    : RecordStoreBase(params.uuid, params.ident),
      _engineName(params.engineName),
      _container(_makeContainer(params)),
      _overwrite(params.overwrite),
      _isLogged(params.isLogged),
      _forceUpdateWithFullDocument(params.forceUpdateWithFullDocument),
      _inMemory(params.inMemory),
      _sizeStorer(params.sizeStorer),
      _tracksSizeAdjustments(params.tracksSizeAdjustments),
      _kvEngine(kvEngine) {
    invariant(getIdent().size() > 0);

    if (kDebugBuild && keyFormat() == KeyFormat::String) {
        // This is a clustered record store. Its WiredTiger table requires key_format='u' for
        // correct operation.
        const std::string wtTableConfig =
            uassertStatusOK(WiredTigerUtil::getMetadataCreate(*ru.getSessionNoTxn(), getURI()));
        const bool wtTableConfigMatchesStringKeyFormat =
            wtTableConfig.find("key_format=u") != std::string::npos;
        invariant(wtTableConfigMatchesStringKeyFormat);
    }

    Status versionStatus =
        WiredTigerUtil::checkApplicationMetadataFormatVersion(
            *ru.getSessionNoTxn(), getURI(), kMinimumRecordStoreVersion, kMaximumRecordStoreVersion)
            .getStatus();

    if (!versionStatus.isOK()) {
        LOGV2_ERROR(7887900,
                    "Metadata format version check failed.",
                    "uri"_attr = getURI(),
                    "uuid"_attr = uuid(),
                    "version"_attr = versionStatus.reason());
        if (versionStatus.code() == ErrorCodes::FailedToParse) {
            uasserted(28548, versionStatus.reason());
        } else {
            fassertFailedNoTrace(34433);
        }
    }

    uassertStatusOK(
        WiredTigerUtil::setTableLogging(*ru.getSession(), std::string{getURI()}, _isLogged));

    // If no SizeStorer is in use, start counting at zero. In practice, this will only ever be the
    // case for temporary RecordStores (those not associated with any collection) and in unit
    // tests. Persistent size information is not required in either case. If a RecordStore needs
    // persistent size information, we require it to use a SizeStorer.
    _sizeInfo = _sizeStorer ? _sizeStorer->load(*ru.getSessionNoTxn(), getURI())
                            : std::make_shared<WiredTigerSizeStorer::SizeInfo>(0, 0);
}

WiredTigerRecordStore::~WiredTigerRecordStore() {
    if (!isTemp()) {
        LOGV2_DEBUG(
            22395, 1, "~WiredTigerRecordStore", "ident"_attr = getIdent(), "uuid"_attr = uuid());
    } else {
        LOGV2_DEBUG(22396,
                    1,
                    "~WiredTigerRecordStore for temporary ident: {getIdent}",
                    "getIdent"_attr = getIdent());
    }
}

RecordStore::RecordStoreContainer WiredTigerRecordStore::getContainer() {
    return std::visit([](auto& v) -> RecordStore::RecordStoreContainer { return v; }, _container);
}

void WiredTigerRecordStore::checkSize(OperationContext* opCtx, RecoveryUnit& ru) {
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, ru, /*forward=*/true);
    if (!cursor->next()) {
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
        LOGV2_FOR_RECOVERY(23983,
                           2,
                           "Record store was empty; setting count metadata to zero but marking "
                           "record store as needing size adjustment during recovery",
                           "uuid"_attr = uuid(),
                           "ident"_attr = getIdent());
        sizeRecoveryState(getGlobalServiceContext())
            .markCollectionAsAlwaysNeedsSizeAdjustment(getIdent());
        _sizeInfo->dataSize.store(0);
        _sizeInfo->numRecords.store(0);
    }

    if (_sizeStorer)
        _sizeStorer->store(getURI(), _sizeInfo);
}

long long WiredTigerRecordStore::dataSize() const {
    auto dataSize = _sizeInfo->dataSize.load();
    return dataSize > 0 ? dataSize : 0;
}

long long WiredTigerRecordStore::numRecords() const {
    auto numRecords = _sizeInfo->numRecords.load();
    return numRecords > 0 ? numRecords : 0;
}

int64_t WiredTigerRecordStore::storageSize(RecoveryUnit& ru,
                                           BSONObjBuilder* extraInfo,
                                           int infoLevel) const {
    if (_inMemory) {
        return dataSize();
    }
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(ru).getSessionNoTxn();
    auto result = WiredTigerUtil::getStatisticsValue(
        *session, "statistics:" + getURI(), "statistics=(size)", WT_STAT_DSRC_BLOCK_SIZE);
    uassertStatusOK(result.getStatus());

    return result.getValue();
}

int64_t WiredTigerRecordStore::Capped::storageSize(RecoveryUnit& ru,
                                                   BSONObjBuilder* extraInfo,
                                                   int infoLevel) const {
    // Many things assume an empty capped collection still takes up space.
    return std::max(WiredTigerRecordStore::storageSize(ru, extraInfo, infoLevel), int64_t{1});
}

int64_t WiredTigerRecordStore::freeStorageSize(RecoveryUnit& ru) const {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(ru).getSessionNoTxn();
    return WiredTigerUtil::getIdentReuseSize(*session, std::string{getURI()});
}

void WiredTigerRecordStore::_updateLargestRecordId(OperationContext* opCtx,
                                                   RecoveryUnit& ru,
                                                   long long largestSeen) {
    invariant(keyFormat() == KeyFormat::Long);

    // Make sure to initialize first; otherwise the compareAndSwap can succeed trivially.
    _initNextIdIfNeeded(opCtx, ru);

    // Since the 'largestSeen' is the largest we've seen, we need to set the _nextIdNum to one
    // higher than that: to 'largestSeen + 1'. This is because if we assign recordIds,
    // we start at _nextIdNum. Therefore if it was set to 'largestSeen', it would clash with
    // the current largest recordId.
    largestSeen++;
    auto nextIdNum = _nextIdNum.load();
    while (largestSeen > nextIdNum && !_nextIdNum.compareAndSwap(&nextIdNum, largestSeen)) {
    }
}

void WiredTigerRecordStore::_deleteRecord(OperationContext* opCtx,
                                          RecoveryUnit& ru,
                                          const RecordId& id) {
    invariant(ru.inUnitOfWork());

    // SERVER-48453: Initialize the next record id counter before deleting. This ensures we won't
    // reuse record ids, which can be problematic for the _mdb_catalog.
    if (keyFormat() == KeyFormat::Long) {
        _initNextIdIfNeeded(opCtx, ru);
    }

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    // getSession() will open a txn if there was no txn active.
    wtRu.getSession();
    wtRu.assertInActiveTxn();

    OpStats opStats{};
    wtDeleteRecord(opCtx, wtRu, id, opStats);

    _changeNumRecordsAndDataSize(wtRu, -1, -opStats.oldValueLength);
}

Status WiredTigerRecordStore::_insertRecords(OperationContext* opCtx,
                                             RecoveryUnit& ru,
                                             std::vector<Record>* records,
                                             const std::vector<Timestamp>& timestamps) {
    invariant(ru.inUnitOfWork());

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    auto cursorParams = getWiredTigerCursorParams(wtRu, tableId(), _overwrite);
    WiredTigerCursor curwrap(std::move(cursorParams), getURI(), *wtRu.getSession());

    wtRu.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    auto nRecords = records->size();
    invariant(nRecords != 0);

    if (keyFormat() == KeyFormat::Long) {
        bool areRecordIdsProvided = !records->front().id.isNull();
        RecordId highestRecordIdProvided;

        long long nextId = areRecordIdsProvided ? 0 : _reserveIdBlock(opCtx, ru, nRecords);

        // Non-clustered record stores will extract the RecordId key for the oplog and generate
        // unique int64_t RecordIds if RecordIds are not set.
        for (size_t i = 0; i < nRecords; i++) {
            auto& record = (*records)[i];
            // Some RecordStores, like TemporaryRecordStores, may want to set their own RecordIds.
            if (!areRecordIdsProvided) {
                // Since a recordId wasn't provided for the first record, the recordId shouldn't
                // have been provided for any record.
                invariant(record.id.isNull());
                record.id = RecordId(nextId++);
                invariant(record.id.isValid());
            } else {
                // Since a recordId was provided for the first record, the recordId should have been
                // provided for all records.
                invariant(!record.id.isNull());
                if (record.id > highestRecordIdProvided) {
                    highestRecordIdProvided = record.id;
                }
            }
        }

        // Update the highest recordId we've seen so far on this record store, in case
        // any of the inserts we are performing has a higher recordId.
        // We only have to do this when the records we are inserting were accompanied
        // by caller provided recordIds.
        if (areRecordIdsProvided) {
            _updateLargestRecordId(opCtx, ru, highestRecordIdProvided.getLong());
        }
    }

    int64_t totalLength = 0;
    for (size_t i = 0; i < nRecords; i++) {
        auto& record = (*records)[i];
        totalLength += record.data.size();
        invariant(!record.id.isNull());
        invariant(!record_id_helpers::isReserved(record.id));
        Timestamp ts = timestamps[i];
        if (!ts.isNull()) {
            LOGV2_DEBUG(22403, 4, "inserting record with timestamp {ts}", "ts"_attr = ts);
            fassert(39001, wtRu.setTimestamp(ts));
        }

        OpStats opStats{};
        Status status = wtInsertRecord(opCtx, wtRu, c, record, opStats);
        if (!status.isOK()) {
            return status;
        }
    }
    _changeNumRecordsAndDataSize(wtRu, nRecords, totalLength);

    return Status::OK();
}

Status WiredTigerRecordStore::_updateRecord(
    OperationContext* opCtx, RecoveryUnit& ru, const RecordId& id, const char* data, int len) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    invariant(ru.inUnitOfWork());

    // getSession() will open a txn if there was no txn active.
    wtRu.getSession();
    wtRu.assertInActiveTxn();

    OpStats opStats{};
    auto status = wtUpdateRecord(opCtx, wtRu, id, data, len, opStats);
    if (!status.isOK()) {
        return status;
    }

    auto sizeDiff = opStats.newValueLength - opStats.oldValueLength;
    _changeNumRecordsAndDataSize(wtRu, 0, sizeDiff);
    return Status::OK();
}

bool WiredTigerRecordStore::updateWithDamagesSupported() const {
    return true;
}

StatusWith<RecordData> WiredTigerRecordStore::_updateWithDamages(OperationContext* opCtx,
                                                                 RecoveryUnit& ru,
                                                                 const RecordId& id,
                                                                 const RecordData& oldRec,
                                                                 const char* damageSource,
                                                                 const DamageVector& damages) {
    const int nentries = damages.size();
    DamageVector::const_iterator where = damages.begin();
    const DamageVector::const_iterator end = damages.cend();
    std::vector<WT_MODIFY> entries(nentries);
    for (u_int i = 0; where != end; ++i, ++where) {
        entries[i].data.data = damageSource + where->sourceOffset;
        entries[i].data.size = where->sourceSize;
        entries[i].offset = where->targetOffset;
        entries[i].size = where->targetSize;
    }

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    auto cursorParams = getWiredTigerCursorParams(wtRu, tableId(), /*allowOverwrite=*/true);
    WiredTigerCursor curwrap(std::move(cursorParams), getURI(), *wtRu.getSession());

    wtRu.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    CursorKey key = makeCursorKey(id, keyFormat());
    setKey(c, &key);

    // The test harness calls us with empty damage vectors which WiredTiger doesn't allow.
    if (nentries == 0)
        invariantWTOK(WT_OP_CHECK(c->search(c)), c->session);
    else
        invariantWTOK(WT_OP_CHECK(wiredTigerCursorModify(wtRu, c, entries.data(), nentries)),
                      c->session);


    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value), c->session);

    auto sizeDiff = static_cast<int64_t>(value.size) - static_cast<int64_t>(oldRec.size());
    _changeNumRecordsAndDataSize(wtRu, 0, sizeDiff);

    return RecordData(static_cast<const char*>(value.data), value.size).getOwned();
}

void WiredTigerRecordStore::printRecordMetadata(const RecordId& recordId,
                                                std::set<Timestamp>* recordTimestamps) const {
    // Printing the record metadata requires a new session. We cannot open other cursors when there
    // are open history store cursors in the session.
    WiredTigerSession session(&_kvEngine->getConnection());

    // Per the version cursor API:
    // - A version cursor can only be called with the read timestamp as the oldest timestamp.
    // - If there is no oldest timestamp, the version cursor can only be called with a read
    //   timestamp of 1.
    Timestamp oldestTs = _kvEngine->getOldestTimestamp();
    const std::string config = fmt::format("read_timestamp={:x},roundup_timestamps=(read=true)",
                                           oldestTs.isNull() ? 1 : oldestTs.asULL());
    WiredTigerBeginTxnBlock beginTxn(&session, config.c_str());

    // Open a version cursor. This is a debug cursor that enables iteration through the history of
    // values for a given record.
    WT_CURSOR* cursor = session.getNewCursor(getURI(), "debug=(dump_version=(enabled=true))");

    CursorKey key = makeCursorKey(recordId, keyFormat());
    setKey(cursor, &key);

    int ret = cursor->search(cursor);
    while (ret != WT_NOTFOUND) {
        invariantWTOK(ret, cursor->session);

        uint64_t startTs = 0, startDurableTs = 0, stopTs = 0, stopDurableTs = 0;
        uint64_t startTxnId = 0, stopTxnId = 0;
        uint8_t flags = 0, location = 0, prepare = 0, type = 0;
        WT_ITEM value;

        invariantWTOK(cursor->get_value(cursor,
                                        &startTxnId,
                                        &startTs,
                                        &startDurableTs,
                                        &stopTxnId,
                                        &stopTs,
                                        &stopDurableTs,
                                        &type,
                                        &prepare,
                                        &flags,
                                        &location,
                                        &value),
                      cursor->session);

        RecordData recordData(static_cast<const char*>(value.data), value.size);
        LOGV2(6120300,
              "WiredTiger record metadata",
              "recordId"_attr = recordId,
              "startTxnId"_attr = startTxnId,
              "startTs"_attr = Timestamp(startTs),
              "startDurableTs"_attr = Timestamp(startDurableTs),
              "stopTxnId"_attr = stopTxnId,
              "stopTs"_attr = Timestamp(stopTs),
              "stopDurableTs"_attr = Timestamp(stopDurableTs),
              "type"_attr = type,
              "prepare"_attr = prepare,
              "flags"_attr = flags,
              "location"_attr = location,
              "value"_attr = redact(recordData.toBson()));

        // Save all relevant timestamps that we just printed.
        if (recordTimestamps) {
            auto saveRecordTimestampIfValid = [recordTimestamps](Timestamp ts) {
                if (ts.isNull() || ts == Timestamp::max() || ts == Timestamp::min()) {
                    return;
                }
                (void)recordTimestamps->emplace(ts);
            };
            saveRecordTimestampIfValid(Timestamp(startTs));
            saveRecordTimestampIfValid(Timestamp(startDurableTs));
            saveRecordTimestampIfValid(Timestamp(stopTs));
            saveRecordTimestampIfValid(Timestamp(stopDurableTs));
        }

        ret = cursor->next(cursor);
    }
}

std::unique_ptr<RecordCursor> WiredTigerRecordStore::getRandomCursor(OperationContext* opCtx,
                                                                     RecoveryUnit& ru) const {
    return std::make_unique<RandomCursor>(opCtx, ru, *this);
}

Status WiredTigerRecordStore::_truncate(OperationContext* opCtx, RecoveryUnit& ru) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    wtTruncate(opCtx, wtRu);
    _changeNumRecordsAndDataSize(wtRu, -numRecords(), -dataSize());
    return Status::OK();
}

Status WiredTigerRecordStore::_rangeTruncate(OperationContext* opCtx,
                                             RecoveryUnit& ru,
                                             const RecordId& minRecordId,
                                             const RecordId& maxRecordId,
                                             int64_t hintDataSizeDiff,
                                             int64_t hintNumRecordsDiff) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    wtRangeTruncate(wtRu, minRecordId, maxRecordId);
    _changeNumRecordsAndDataSize(wtRu, hintNumRecordsDiff, hintDataSizeDiff);
    return Status::OK();
}

StatusWith<int64_t> WiredTigerRecordStore::_compact(OperationContext* opCtx,
                                                    RecoveryUnit& ru,
                                                    const CompactOptions& options) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    return wtCompact(opCtx, wtRu, options);
}

void WiredTigerRecordStore::validate(RecoveryUnit& ru,
                                     const CollectionValidation::ValidationOptions& options,
                                     ValidateResults* results) {
    if (_inMemory) {
        return;
    }

    WiredTigerUtil::validateTableLogging(*WiredTigerRecoveryUnit::get(ru).getSessionNoTxn(),
                                         getURI(),
                                         _isLogged,
                                         boost::none,
                                         *results);

    if (!options.isFullValidation()) {
        invariant(!options.verifyConfigurationOverride().has_value());
        return;
    }

    int err = WiredTigerUtil::verifyTable(*WiredTigerRecoveryUnit::get(ru).getSession(),
                                          std::string{getURI()},
                                          options.verifyConfigurationOverride(),
                                          results->getErrorsUnsafe());
    if (!err) {
        return;
    }

    if (err == EBUSY) {
        std::string msg = str::stream()
            << "Could not complete validation of " << getURI() << ". "
            << "This is a transient issue as the collection was actively "
               "in use by other operations.";

        LOGV2_PROD_ONLY(
            22408,
            "Could not complete validation, This is a transient issue as the collection "
            "was actively in use by other operations",
            "uri"_attr = getURI());
        results->addWarning(msg);
        return;
    }

    const char* errorStr = wiredtiger_strerror(err);
    std::string msg = str::stream() << "verify() returned " << errorStr << ". "
                                    << "This indicates structural damage. "
                                    << "Not examining individual documents.";
    LOGV2_ERROR(22409,
                "Verification returned error. This indicates structural damage. Not examining "
                "individual documents",
                "error"_attr = errorStr);
    results->addError(msg);
}

void WiredTigerRecordStore::appendNumericCustomStats(RecoveryUnit& ru,
                                                     BSONObjBuilder* result,
                                                     double scale) const {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(ru).getSessionNoTxn();

    BSONObjBuilder bob(result->subobjStart(_engineName));

    appendNumericStats(*session, std::string{getURI()}, bob);
}

void WiredTigerRecordStore::appendAllCustomStats(RecoveryUnit& ru,
                                                 BSONObjBuilder* result,
                                                 double scale) const {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(ru).getSessionNoTxn();
    BSONObjBuilder bob(result->subobjStart(_engineName));
    {
        BSONObjBuilder metadata(bob.subobjStart("metadata"));
        Status status = WiredTigerUtil::getApplicationMetadata(*session, getURI(), &metadata);
        if (!status.isOK()) {
            metadata.append("error", "unable to retrieve metadata");
            metadata.append("code", static_cast<int>(status.code()));
            metadata.append("reason", status.reason());
        }
    }

    std::string type, sourceURI;
    WiredTigerUtil::fetchTypeAndSourceURI(*session, std::string{getURI()}, &type, &sourceURI);
    StatusWith<std::string> metadataResult = WiredTigerUtil::getMetadataCreate(*session, sourceURI);
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

    appendNumericStats(*session, std::string{getURI()}, bob);
}

void WiredTigerRecordStore::updateStatsAfterRepair(long long numRecords, long long dataSize) {
    // We're correcting the size as of now, future writes should be tracked.
    sizeRecoveryState(getGlobalServiceContext())
        .markCollectionAsAlwaysNeedsSizeAdjustment(getIdent());

    _sizeInfo->numRecords.store(std::max(numRecords, 0ll));
    _sizeInfo->dataSize.store(std::max(dataSize, 0ll));

    // If we have a WiredTigerSizeStorer, but our size info is not currently cached, add it.
    if (_sizeStorer)
        _sizeStorer->store(getURI(), _sizeInfo);
}

void WiredTigerRecordStore::_initNextIdIfNeeded(OperationContext* opCtx, RecoveryUnit& ru) {
    // Clustered record stores do not automatically generate int64 RecordIds. RecordIds are instead
    // constructed as binary strings, KeyFormat::String, from the user-defined cluster key.
    invariant(keyFormat() == KeyFormat::Long);

    // In the normal case, this will already be initialized, so use a weak load. Since this value
    // will only change from 0 to a positive integer, the only risk is reading an outdated value, 0,
    // and having to take the mutex.
    if (_nextIdNum.loadRelaxed() > 0) {
        return;
    }

    // Only one thread needs to do this.
    stdx::lock_guard<stdx::mutex> lk(_initNextIdMutex);
    if (_nextIdNum.load() > 0) {
        return;
    }

    // During startup recovery, the collectionAlwaysNeedsSizeAdjustment flag is not set by default
    // for the sake of efficiency. However, if we reach this point, we may need to set it in order
    // to ensure that capped deletes can occur on documents inserted earlier in startup recovery.
    if (InReplicationRecovery::isSet(opCtx->getServiceContext()) &&
        !sizeRecoveryState(opCtx->getServiceContext())
             .collectionAlwaysNeedsSizeAdjustment(getIdent())) {
        checkSize(opCtx, ru);
    }

    // Find the largest RecordId in the table and add 1 to generate our next RecordId. The
    // largest_key API returns the largest key in the table regardless of visibility. This ensures
    // we don't re-use RecordIds that are not visible.

    // Need to start at 1 so we are always higher than RecordId::minLong(). This will be the case if
    // the table is empty, and returned RecordId is null.
    int64_t nextId = getLargestKey(opCtx, ru).getLong() + 1;
    _nextIdNum.store(nextId);
}

RecordId WiredTigerRecordStore::getLargestKey(OperationContext* opCtx, RecoveryUnit& ru) const {
    // Initialize the highest seen RecordId in a session without a read timestamp because that is
    // required by the largest_key API.
    WiredTigerSession sessRaii(&_kvEngine->getConnection());

    if (ru.inUnitOfWork()) {
        // We must limit the amount of time spent blocked on cache eviction to avoid a deadlock with
        // ourselves. The calling operation may have a session open that has written a large amount
        // of data, and by creating a new session, we are preventing WT from being able to roll back
        // that transaction to free up cache space. If we do block on cache eviction here, we must
        // consider that the other session owned by this thread may be the one that needs to be
        // rolled back. If this does time out, we will receive a WT_ROLLBACK and throw an error.
        invariantWTOK(sessRaii.reconfigure("cache_max_wait_ms=1000"), sessRaii);
    }

    auto cursor = sessRaii.getNewCursor(getURI());
    int ret = cursor->largest_key(cursor);
    if (ret == WT_ROLLBACK) {
        // Force the caller to rollback its transaction if we can't make progress with eviction.
        // TODO (SERVER-105908): Convert this to a different error code that is distinguishable from
        // a true write conflict.
        int err, sub_level_err;
        const char* err_msg;
        sessRaii.get_last_error(&err, &sub_level_err, &err_msg);
        LOGV2_DEBUG(9979800,
                    2,
                    "WiredTigerRecordStore: rolling back change to numRecords and dataSize",
                    "err"_attr = err,
                    "sub_level_err"_attr = sub_level_err,
                    "err_msg"_attr = err_msg);

        throwWriteConflictException(
            fmt::format("Rollback occurred while performing initial write to '{}'. Reason: '{}'",
                        uuid() ? uuid()->toString() : std::string{},
                        err_msg));
    } else if (ret != WT_NOTFOUND) {
        if (ret == ENOTSUP) {
            auto creationMetadata =
                WiredTigerUtil::getMetadataCreate(sessRaii, getURI()).getValue();
            if (creationMetadata.find("lsm=") != std::string::npos) {
                LOGV2_FATAL(
                    6627200,
                    "WiredTiger tables using 'type=lsm' (Log-Structured Merge Tree) are not "
                    "supported.",
                    "uuid"_attr = uuid(),
                    "metadata"_attr = redact(creationMetadata));
            }
        }
        invariantWTOK(ret, sessRaii);
        return getKey(cursor, keyFormat());
    }
    // Empty table.
    return RecordId();
}

void WiredTigerRecordStore::reserveRecordIds(OperationContext* opCtx,
                                             RecoveryUnit& ru,
                                             std::vector<RecordId>* out,
                                             size_t nRecords) {
    auto nextId = _reserveIdBlock(opCtx, ru, nRecords);
    for (size_t i = 0; i < nRecords; i++) {
        out->push_back(RecordId(nextId++));
    }
}

long long WiredTigerRecordStore::_reserveIdBlock(OperationContext* opCtx,
                                                 RecoveryUnit& ru,
                                                 size_t nRecords) {
    // Clustered record stores do not automatically generate int64 RecordIds. RecordIds are instead
    // constructed as binary strings, KeyFormat::String, from the user-defined cluster key.
    invariant(keyFormat() == KeyFormat::Long);
    _initNextIdIfNeeded(opCtx, ru);
    return _nextIdNum.fetchAndAdd(nRecords);
}

void WiredTigerRecordStore::_changeNumRecordsAndDataSize(RecoveryUnit& ru,
                                                         int64_t numRecordDiff,
                                                         int64_t dataSizeDiff) {
    if (numRecordDiff == 0 && dataSizeDiff == 0) {
        // If there's nothing to increment/decrement this will be a no-op. Avoid all the other
        // checks and early return.
        return;
    }

    if (!_tracksSizeAdjustments) {
        return;
    }

    if (!sizeRecoveryState(getGlobalServiceContext()).collectionNeedsSizeAdjustment(getIdent())) {
        return;
    }

    const auto updateAndStoreSizeInfo = [this](int64_t numRecordDiff, int64_t dataSizeDiff) {
        _sizeInfo->numRecords.addAndFetch(numRecordDiff);
        _sizeInfo->dataSize.addAndFetch(dataSizeDiff);

        if (_sizeStorer)
            _sizeStorer->store(getURI(), _sizeInfo);
    };

    ru.onRollback([updateAndStoreSizeInfo, numRecordDiff, dataSizeDiff](auto _) {
        LOGV2_DEBUG(7105300,
                    2,
                    "WiredTigerRecordStore: rolling back change to numRecords and dataSize",
                    "numRecordDiff"_attr = -numRecordDiff,
                    "dataSizeDiff"_attr = -dataSizeDiff);
        updateAndStoreSizeInfo(-numRecordDiff, -dataSizeDiff);
    });
    updateAndStoreSizeInfo(numRecordDiff, dataSizeDiff);
}

void WiredTigerRecordStore::setNumRecords(long long numRecords) {
    _sizeInfo->numRecords.store(std::max(numRecords, 0ll));

    if (!_sizeStorer) {
        return;
    }

    // Flush the updated number of records to disk immediately.
    _sizeStorer->store(getURI(), _sizeInfo);
    bool syncToDisk = true;
    _sizeStorer->flush(syncToDisk);
}

void WiredTigerRecordStore::setDataSize(long long dataSize) {
    _sizeInfo->dataSize.store(std::max(dataSize, 0ll));

    if (!_sizeStorer) {
        return;
    }

    // Flush the updated data size to disk immediately.
    _sizeStorer->store(getURI(), _sizeInfo);
    bool syncToDisk = true;
    _sizeStorer->flush(syncToDisk);
}

std::unique_ptr<SeekableRecordCursor> WiredTigerRecordStore::getCursor(OperationContext* opCtx,
                                                                       RecoveryUnit& ru,
                                                                       bool forward) const {
    auto cursor = std::make_unique<WiredTigerRecordStoreCursor>(opCtx, ru, *this, forward);
    cursor->init();
    return cursor;
}

RecordStore::Capped* WiredTigerRecordStore::capped() {
    return nullptr;
}

RecordStore::Capped* WiredTigerRecordStore::Capped::capped() {
    return this;
}

RecordStore::Capped* WiredTigerRecordStore::Oplog::capped() {
    return this;
}

RecordStore::Oplog* WiredTigerRecordStore::oplog() {
    return nullptr;
}

RecordStore::Oplog* WiredTigerRecordStore::Oplog::oplog() {
    return this;
}

WiredTigerRecordStore::Capped::Capped(WiredTigerKVEngine* engine,
                                      WiredTigerRecoveryUnit& ru,
                                      Params params)
    : WiredTigerRecordStore(engine, ru, params) {}

std::unique_ptr<SeekableRecordCursor> WiredTigerRecordStore::Capped::getCursor(
    OperationContext* opCtx, RecoveryUnit& ru, bool forward) const {
    return std::make_unique<WiredTigerStandardCappedCursor>(opCtx, ru, *this, forward);
}

RecordStore::Capped::TruncateAfterResult WiredTigerRecordStore::Capped::_truncateAfter(
    OperationContext* opCtx, RecoveryUnit& ru, const RecordId& end, bool inclusive) {
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, ru, true);

    auto record = cursor->seekExact(end);
    massert(28807, str::stream() << "Failed to seek to the record located at " << end, record);

    int64_t recordsRemoved = 0;
    int64_t bytesRemoved = 0;
    RecordId lastKeptId;
    RecordId firstRemovedId;

    if (inclusive) {
        std::unique_ptr<SeekableRecordCursor> reverseCursor = getCursor(opCtx, ru, false);
        invariant(reverseCursor->seekExact(end));
        auto prev = reverseCursor->next();
        lastKeptId = prev ? std::move(prev->id) : RecordId();
        firstRemovedId = end;
    } else {
        // If not deleting the record located at 'end', then advance the cursor to the first record
        // that is being deleted.
        record = cursor->next();
        if (!record) {
            return {};  // No records to delete.
        }
        lastKeptId = end;
        firstRemovedId = record->id;
    }

    // Compute the number and associated sizes of the records to delete.
    do {
        recordsRemoved++;
        bytesRemoved += record->data.size();
    } while ((record = cursor->next()));

    // Truncate the collection starting from the record located at 'firstRemovedId' to the end of
    // the collection.

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    auto cursorParams = getWiredTigerCursorParams(wtRu, tableId(), /*allowOverwrite=*/true);
    WiredTigerCursor startwrap(std::move(cursorParams), getURI(), *wtRu.getSession());

    WT_CURSOR* start = startwrap.get();
    auto key = makeCursorKey(firstRemovedId, keyFormat());
    setKey(start, &key);

    WiredTigerSession* session = WiredTigerRecoveryUnit::get(ru).getSession();
    invariantWTOK(session->truncate(nullptr, start, nullptr, nullptr), *session);

    _changeNumRecordsAndDataSize(ru, -recordsRemoved, -bytesRemoved);

    _handleTruncateAfter(WiredTigerRecoveryUnit::get(wtRu), lastKeptId);

    return {recordsRemoved, bytesRemoved, std::move(firstRemovedId)};
}

void WiredTigerRecordStore::Capped::_handleTruncateAfter(WiredTigerRecoveryUnit&,
                                                         const RecordId& lastKeptId) {}

void WiredTigerRecordStore::Oplog::_handleTruncateAfter(WiredTigerRecoveryUnit& ru,
                                                        const RecordId& lastKeptId) {
    // Immediately rewind visibility to our truncation point, to prevent new
    // transactions from appearing.
    Timestamp truncTs(lastKeptId.getLong());

    auto conn = ru.getConnection()->conn();
    auto durableTSConfigString = fmt::format("durable_timestamp={:x}", truncTs.asULL());
    invariantWTOK(conn->set_timestamp(conn, durableTSConfigString.c_str()), *ru.getSession());

    _kvEngine->getOplogManager()->setOplogReadTimestamp(truncTs);
    LOGV2_DEBUG(22405, 1, "Truncation new read timestamp", "ts"_attr = truncTs);
}

WiredTigerRecordStore::Oplog::Oplog(WiredTigerKVEngine* engine,
                                    WiredTigerRecoveryUnit& ru,
                                    Params oplogParams)
    : Capped(engine,
             ru,
             {.uuid = oplogParams.uuid,
              .ident = oplogParams.ident,
              .engineName = oplogParams.engineName,
              .keyFormat = KeyFormat::Long,
              .overwrite = true,
              .isLogged = oplogParams.isLogged,
              .forceUpdateWithFullDocument = oplogParams.forceUpdateWithFullDocument,
              .inMemory = oplogParams.inMemory,
              .sizeStorer = oplogParams.sizeStorer,
              .tracksSizeAdjustments = oplogParams.tracksSizeAdjustments}),
      _maxSize(oplogParams.oplogMaxSize) {
    invariant(WiredTigerRecordStore::keyFormat() == KeyFormat::Long);
    invariant(oplogParams.oplogMaxSize);
    checkOplogFormatVersion(ru, std::string{getURI()});
    // The oplog always needs to be marked for size adjustment since it is journaled and also
    // may change during replication recovery (if truncated).
    sizeRecoveryState(getGlobalServiceContext())
        .markCollectionAsAlwaysNeedsSizeAdjustment(WiredTigerRecordStore::getIdent());
}

std::unique_ptr<SeekableRecordCursor> WiredTigerRecordStore::Oplog::getRawCursor(
    OperationContext* opCtx, RecoveryUnit& ru, bool forward) const {
    auto cursor = std::make_unique<WiredTigerRecordStoreCursor>(opCtx, ru, *this, forward);
    cursor->init();
    return cursor;
}

WiredTigerRecordStore::Oplog::~Oplog() {
    _kvEngine->getOplogManager()->stop();
}

std::unique_ptr<SeekableRecordCursor> WiredTigerRecordStore::Oplog::getCursor(
    OperationContext* opCtx, RecoveryUnit& ru, bool forward) const {
    return std::make_unique<WiredTigerOplogCursor>(opCtx, ru, *this, forward);
}

void WiredTigerRecordStore::Oplog::validate(RecoveryUnit& ru,
                                            const CollectionValidation::ValidationOptions& options,
                                            ValidateResults* results) {
    if (_inMemory) {
        return;
    }

    WiredTigerUtil::validateTableLogging(*WiredTigerRecoveryUnit::get(ru).getSessionNoTxn(),
                                         getURI(),
                                         _isLogged,
                                         boost::none,
                                         *results);

    if (!options.isFullValidation()) {
        invariant(!options.verifyConfigurationOverride().has_value());
        return;
    }

    results->addWarning("Skipping verification of the WiredTiger table for the oplog.");
}

Status WiredTigerRecordStore::Oplog::updateSize(long long newOplogSize) {
    invariant(newOplogSize);
    if (_maxSize.load() != newOplogSize) {
        _maxSize.store(newOplogSize);
    }
    return Status::OK();
}

int64_t WiredTigerRecordStore::Oplog::getMaxSize() const {
    return _maxSize.load();
}

StatusWith<Timestamp> WiredTigerRecordStore::Oplog::getLatestTimestamp(RecoveryUnit& ru) const {
    // Using this function inside a UOW is not supported because the main reason to call it is to
    // synchronize to the last op before waiting for write concern, so it makes little sense to do
    // so in a UOW. This also ensures we do not return uncommitted entries.
    invariant(!ru.inUnitOfWork());

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    bool ruWasActive = wtRu.isActive();

    // getSession will open a txn if there was no txn active.
    auto session = wtRu.getSession();

    ON_BLOCK_EXIT([&] {
        if (!ruWasActive) {
            // In case the RU was inactive, leave it in that state.
            wtRu.abandonSnapshot();
        }
    });

    auto cachedCursor = session->getCachedCursor(tableId(), "");
    auto cursor = cachedCursor ? cachedCursor : session->getNewCursor(getURI());
    ON_BLOCK_EXIT([&] { session->releaseCursor(tableId(), cursor, ""); });
    int ret = cursor->prev(cursor);
    if (ret == WT_NOTFOUND) {
        return Status(ErrorCodes::CollectionIsEmpty, "oplog is empty");
    }
    invariantWTOK(ret, cursor->session);

    RecordId recordId = getKey(cursor, keyFormat());

    return {Timestamp(static_cast<unsigned long long>(recordId.getLong()))};
}

StatusWith<Timestamp> WiredTigerRecordStore::Oplog::getEarliestTimestamp(RecoveryUnit& ru) {
    auto wtRu = WiredTigerRecoveryUnit::get(&ru);

    auto cursorParams = getWiredTigerCursorParams(*wtRu, tableId(), /*allowOverwrite=*/true);
    WiredTigerCursor curwrap(std::move(cursorParams), getURI(), *wtRu->getSession());

    auto cursor = curwrap.get();
    auto ret = cursor->next(cursor);
    if (ret == WT_NOTFOUND) {
        return Status(ErrorCodes::CollectionIsEmpty, "oplog is empty");
    }
    invariantWTOK(ret, cursor->session);

    auto firstRecord = getKey(cursor, KeyFormat::Long);
    return Timestamp(static_cast<uint64_t>(firstRecord.getLong()));
}

Status WiredTigerRecordStore::Oplog::_insertRecords(OperationContext* opCtx,
                                                    RecoveryUnit& ru,
                                                    std::vector<Record>* records,
                                                    const std::vector<Timestamp>& timestamps) {
    invariant(ru.inUnitOfWork());

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    auto cursorParams = getWiredTigerCursorParams(wtRu, tableId(), _overwrite);
    WiredTigerCursor curwrap(std::move(cursorParams), getURI(), *wtRu.getSession());

    wtRu.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    invariant(!records->empty());
    auto nRecords = records->size();

    // Non-clustered record stores will extract the RecordId key for the oplog and generate
    // unique int64_t RecordIds if RecordIds are not set.
    for (size_t i = 0; i < nRecords; i++) {
        auto& record = (*records)[i];
        auto swRecordId = record_id_helpers::keyForOptime(timestamps[i], KeyFormat::Long);
        if (!swRecordId.isOK())
            return swRecordId.getStatus();

        // In the normal write paths, a timestamp is always set. It is only in unusual cases
        // like inserting the oplog seed document where the caller does not provide a
        // timestamp.
        if (MONGO_unlikely(timestamps[i].isNull() || kDebugBuild)) {
            auto swRecordIdFromBSON =
                record_id_helpers::extractKeyOptime(record.data.data(), record.data.size());
            if (!swRecordIdFromBSON.isOK())
                return swRecordIdFromBSON.getStatus();

            // Double-check that the 'ts' field in the oplog entry matches the assigned
            // timestamp, if it was provided.
            dassert(timestamps[i].isNull() ||
                        swRecordIdFromBSON.getValue() == swRecordId.getValue(),
                    fmt::format("ts field in oplog entry {} does not equal assigned timestamp {}",
                                swRecordIdFromBSON.getValue().toString(),
                                swRecordId.getValue().toString()));

            record.id = std::move(swRecordIdFromBSON.getValue());
        } else {
            record.id = std::move(swRecordId.getValue());
        }
        // The records being inserted into the oplog must have increasing
        // recordId. Therefore the last record has the highest recordId.
        dassert(i == 0 || (*records)[i].id > (*records)[i - 1].id);
    }

    int64_t totalLength = 0;
    for (size_t i = 0; i < nRecords; i++) {
        auto& record = (*records)[i];
        totalLength += record.data.size();
        invariant(!record.id.isNull());
        invariant(!record_id_helpers::isReserved(record.id));
        Timestamp ts = timestamps[i];

        // Setting this transaction to be unordered will trigger a journal flush. Because these
        // are direct writes into the oplog, the machinery to trigger a journal flush is
        // bypassed. A followup oplog read will require a fres value to make progress.
        ru.setOrderedCommit(false);
        auto oplogKeyTs = Timestamp(record.id.getLong());
        if (!ts.isNull()) {
            invariant(oplogKeyTs == ts);
        }
        if (!ru.getCommitTimestamp().isNull()) {
            invariant(oplogKeyTs == ru.getCommitTimestamp());
        }

        if (!ts.isNull()) {
            LOGV2_DEBUG(4017300, 4, "Inserting oplog record with timestamp", "ts"_attr = ts);
            fassert(4017301, ru.setTimestamp(ts));
        }
        CursorKey key = makeCursorKey(record.id, WiredTigerRecordStore::keyFormat());
        setKey(c, &key);
        WiredTigerItem value(record.data.data(), record.data.size());
        c->set_value(c, value.get());
        int ret = WT_OP_CHECK(wiredTigerCursorInsert(WiredTigerRecoveryUnit::get(ru), c));
        invariant(ret != WT_DUPLICATE_KEY);
        if (ret)
            return wtRCToStatus(ret, c->session, "WiredTigerRecordStore::insertRecord");
    }
    _changeNumRecordsAndDataSize(wtRu, nRecords, totalLength);

    return Status::OK();
}

WiredTigerRecordStoreCursorBase::WiredTigerRecordStoreCursorBase(OperationContext* opCtx,
                                                                 RecoveryUnit& ru,
                                                                 const WiredTigerRecordStore& rs,
                                                                 bool forward)
    : _tableId(rs.tableId()),
      _opCtx(opCtx),
      _ru(&ru),
      _uri(rs.getURI()),
      _ident(rs.getIdent()),
      _keyFormat(rs.keyFormat()),
      _forward(forward),
      _uuid(rs.uuid()),
      _assertOutOfOrderForTest(MONGO_unlikely(WTRecordStoreUassertOutOfOrder.shouldFail())) {}

void WiredTigerRecordStoreCursorBase::init() {
    auto& wtRu = WiredTigerRecoveryUnit::get(*_ru);
    auto cursorParams = getWiredTigerCursorParams(wtRu, _tableId, true /* allowOverwrite */);
    _cursor.emplace(std::move(cursorParams), _uri, *wtRu.getSession());
}

boost::optional<Record> WiredTigerRecordStoreCursorBase::next() {
    auto id = nextIdCommon();
    if (id.isNull()) {
        return boost::none;
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    checkOrder(toReturn.id);
    trackReturn(toReturn);
    return toReturn;
}

RecordId WiredTigerRecordStoreCursorBase::nextIdCommon() {
    invariant(_hasRestored);
    if (_eof)
        return {};

    // Ensure an active transaction is open. While WiredTiger supports using cursors on a session
    // without an active transaction (i.e. an implicit transaction), that would bypass configuration
    // options we pass when we explicitly start transactions in the RecoveryUnit.
    WiredTigerRecoveryUnit::get(*_ru).getSession();

    WT_CURSOR* c = _cursor->get();

    RecordId id;
    if (!_skipNextAdvance) {
        // Nothing after the next line can throw WCEs.
        // Note that an unpositioned (or eof) WT_CURSOR returns the first/last entry in the
        // table when you call next/prev.
        int advanceRet = wiredTigerPrepareConflictRetry(
            *_opCtx, StorageExecutionContext::get(_opCtx)->getPrepareConflictTracker(), *_ru, [&] {
                return _forward ? c->next(c) : c->prev(c);
            });
        if (advanceRet == WT_NOTFOUND) {
            _eof = true;
            _positioned = false;
            return {};
        }
        invariantWTOK(advanceRet, c->session);
        id = getKey(c, _keyFormat);
    } else if (!id.isValid()) {
        id = getKey(c, _keyFormat);
    }

    _positioned = true;
    _skipNextAdvance = false;
    return id;
}

void WiredTigerRecordStoreCursorBase::reportOutOfOrderRead(const RecordId& id,
                                                           bool failWithOutOfOrderForTest) const {
    if (!failWithOutOfOrderForTest) {
        // Crash when testing diagnostics are enabled and not explicitly uasserting on
        // out-of-order keys.
        invariant(!TestingProctor::instance().isEnabled(), "cursor returned out-of-order keys");
    }

    auto options = [&] {
        if (_ru->getDataCorruptionDetectionMode() == DataCorruptionDetectionMode::kThrow) {
            // uassert with 'DataCorruptionDetected' after logging.
            return logv2::LogOptions{logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)};
        } else {
            return logv2::LogOptions(logv2::LogComponent::kAutomaticDetermination);
        }
    }();

    LOGV2_ERROR_OPTIONS(22406,
                        options,
                        "WT_Cursor::next -- returned out-of-order keys",
                        "forward"_attr = _forward,
                        "next"_attr = id,
                        "last"_attr = _lastReturnedId,
                        "ident"_attr = _ident,
                        "uuid"_attr = _uuid);
}

void WiredTigerRecordStoreCursorBase::checkOrder(const RecordId& id) const {
    if (MONGO_unlikely((_forward && _lastReturnedId >= id) ||
                       (!_forward && !_lastReturnedId.isNull() && id >= _lastReturnedId) ||
                       _assertOutOfOrderForTest)) {
        reportOutOfOrderRead(id, _assertOutOfOrderForTest);
    }
}

void WiredTigerRecordStoreCursorBase::trackReturn(const Record& record) {
    _lastReturnedId = record.id;
}

void WiredTigerRecordStoreCursorBase::resetCursor() {
    if (_cursor) {
        WT_CURSOR* c = _cursor->get();
        invariantWTOK(WT_READ_CHECK(c->reset(c)), c->session);
        _boundSet = false;
        _positioned = false;
    }
}

boost::optional<Record> WiredTigerRecordStoreCursorBase::seek(const RecordId& start,
                                                              BoundInclusion boundInclusion) {
    auto id = seekIdCommon(start, boundInclusion);
    if (id.isNull()) {
        return boost::none;
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    trackReturn(toReturn);
    return toReturn;
}

RecordId WiredTigerRecordStoreCursorBase::seekIdCommon(const RecordId& start,
                                                       BoundInclusion boundInclusion,
                                                       bool restoring) {
    invariant(_hasRestored || restoring);

    // Ensure an active transaction is open.
    auto session = WiredTigerRecoveryUnit::get(*_ru).getSession();
    _skipNextAdvance = false;

    // If the cursor is positioned, we need to reset it so that we can set bounds. This is not the
    // common use case.
    if (_positioned) {
        resetCursor();
    }

    WT_CURSOR* c = _cursor->get();
    WiredTigerRecordStore::CursorKey key = makeCursorKey(start, _keyFormat);
    setKey(c, &key);

    auto const& config = _forward
        ? (boundInclusion == BoundInclusion::kInclude ? lowerInclusiveBoundConfig
                                                      : lowerExclusiveBoundConfig)
        : (boundInclusion == BoundInclusion::kInclude ? upperInclusiveBoundConfig
                                                      : upperExclusiveBoundConfig);

    invariantWTOK(c->bound(c, config.getConfig(session)), c->session);
    _boundSet = true;

    int ret = wiredTigerPrepareConflictRetry(
        *_opCtx, StorageExecutionContext::get(_opCtx)->getPrepareConflictTracker(), *_ru, [&] {
            return _forward ? c->next(c) : c->prev(c);
        });
    if (ret == WT_NOTFOUND) {
        _eof = true;
        return {};
    }
    invariantWTOK(ret, c->session);

    _positioned = true;
    _eof = false;
    return getKey(c, _keyFormat);
}

boost::optional<Record> WiredTigerRecordStoreCursorBase::seekExact(const RecordId& id) {
    return seekExactCommon(id);
}

boost::optional<Record> WiredTigerRecordStoreCursorBase::seekExactCommon(const RecordId& id) {
    invariant(_hasRestored);

    // Ensure an active transaction is open. While WiredTiger supports using cursors on a session
    // without an active transaction (i.e. an implicit transaction), that would bypass configuration
    // options we pass when we explicitly start transactions in the RecoveryUnit.
    auto session = WiredTigerRecoveryUnit::get(*_ru).getSession();

    _skipNextAdvance = false;
    WT_CURSOR* c = _cursor->get();

    // Before calling WT search, clear any saved bounds from a previous seek.
    if (_boundSet) {
        invariantWTOK(c->bound(c, clearBoundConfig.getConfig(session)), c->session);
        _boundSet = false;
    }

    auto key = makeCursorKey(id, _keyFormat);
    setKey(c, &key);
    // Nothing after the next line can throw WCEs.
    int seekRet = wiredTigerPrepareConflictRetry(
        *_opCtx, StorageExecutionContext::get(_opCtx)->getPrepareConflictTracker(), *_ru, [&] {
            return c->search(c);
        });
    if (seekRet == WT_NOTFOUND) {
        _eof = true;
        return {};
    }
    invariantWTOK(seekRet, c->session);

    _eof = false;
    _positioned = true;
    Record toReturn = {id, getRecordData(c)};
    trackReturn(toReturn);
    return toReturn;
}

void WiredTigerRecordStoreCursorBase::save() {
    resetCursor();
    _ru = nullptr;
    _hasRestored = false;
}

void WiredTigerRecordStoreCursorBase::saveUnpositioned() {
    save();
    _lastReturnedId = RecordId();
}

bool WiredTigerRecordStoreCursorBase::restore(RecoveryUnit& ru, bool tolerateCappedRepositioning) {
    _ru = &ru;

    if (!_cursor) {
        auto& wtRu = WiredTigerRecoveryUnit::get(*_ru);
        auto cursorParams = getWiredTigerCursorParams(wtRu, _tableId, true /* allowOverwrite */);
        _cursor.emplace(std::move(cursorParams), _uri, *wtRu.getSession());
    }

    // This will ensure an active session exists, so any restored cursors will bind to it
    invariant(WiredTigerRecoveryUnit::get(*_ru).getSession() == _cursor->getSession());

    // If we've hit EOF, then this iterator is done and need not be restored.
    if (_eof || _lastReturnedId.isNull()) {
        _hasRestored = true;
        return true;
    }

    auto foundId = seekIdCommon(_lastReturnedId, BoundInclusion::kInclude, true /* restoring */);
    _hasRestored = true;
    if (foundId.isNull()) {
        _eof = true;
        return true;
    }

    int cmp = foundId.compare(_lastReturnedId);
    if (cmp == 0) {
        return true;  // Landed right where we left off.
    }

    // With bounded cursors, we should always find a key greater than the one we searched for.
    dassert(_forward ? cmp > 0 : cmp < 0);

    // We landed after where we were. Return our new location on the next call to next().
    _skipNextAdvance = true;
    return true;
}

void WiredTigerRecordStoreCursorBase::detachFromOperationContext() {
    _opCtx = nullptr;
    if (!_saveStorageCursorOnDetachFromOperationContext) {
        _cursor = boost::none;
    }
}

void WiredTigerRecordStoreCursorBase::reattachToOperationContext(OperationContext* opCtx) {
    _opCtx = opCtx;
    // _cursor recreated in restore() to avoid risk of WT_ROLLBACK issues.
}

WiredTigerRecordStoreCursor::WiredTigerRecordStoreCursor(OperationContext* opCtx,
                                                         RecoveryUnit& ru,
                                                         const WiredTigerRecordStore& rs,
                                                         bool forward)
    : WiredTigerRecordStoreCursorBase(opCtx, ru, rs, forward) {}

WiredTigerCappedCursorBase::WiredTigerCappedCursorBase(OperationContext* opCtx,
                                                       RecoveryUnit& ru,
                                                       const WiredTigerRecordStore& rs,
                                                       bool forward)
    : WiredTigerRecordStoreCursor(opCtx, ru, rs, forward) {}

boost::optional<Record> WiredTigerCappedCursorBase::seekExact(const RecordId& id) {
    if (!isVisible(id)) {
        _eof = true;
        return {};
    }

    return seekExactCommon(id);
}

boost::optional<Record> WiredTigerCappedCursorBase::seek(const RecordId& start,
                                                         BoundInclusion boundInclusion) {
    auto id = seekIdCommon(start, boundInclusion);
    if (id.isNull()) {
        return boost::none;
    }

    if (!isVisible(id)) {
        _eof = true;
        return {};
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    trackReturn(toReturn);
    return toReturn;
}

boost::optional<Record> WiredTigerCappedCursorBase::next() {
    auto id = nextIdCommon();
    if (id.isNull()) {
        return boost::none;
    }

    if (!isVisible(id)) {
        _eof = true;
        return {};
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    checkOrder(toReturn.id);
    trackReturn(toReturn);
    return toReturn;
}

void WiredTigerCappedCursorBase::save() {
    WiredTigerRecordStoreCursor::save();
    resetVisibility();
}

bool WiredTigerCappedCursorBase::restore(RecoveryUnit& ru, bool tolerateCappedRepositioning) {
    _ru = &ru;

    if (!_cursor) {
        auto& wtRu = WiredTigerRecoveryUnit::get(*_ru);
        auto cursorParams = getWiredTigerCursorParams(wtRu, _tableId, true /* allowOverwrite */);
        _cursor.emplace(std::move(cursorParams), _uri, *wtRu.getSession());
    }

    initVisibility();

    // This will ensure an active session exists, so any restored cursors will bind to it
    invariant(WiredTigerRecoveryUnit::get(*_ru).getSession() == _cursor->getSession());

    // If we've hit EOF, then this iterator is done and need not be restored.
    if (_eof || _lastReturnedId.isNull()) {
        _hasRestored = true;
        return true;
    }

    auto foundId = seekIdCommon(_lastReturnedId, BoundInclusion::kInclude, true /* restoring */);
    _hasRestored = true;
    if (foundId.isNull()) {
        _eof = true;
        // Capped read collscans do not tolerate cursor repositioning. By contrast, write collscans
        // on a clustered collection like TTL deletion tolerate cursor repositioning like normal
        // collections.
        if (!tolerateCappedRepositioning) {
            return false;
        }
        return true;
    }

    int cmp = foundId.compare(_lastReturnedId);
    if (cmp == 0) {
        return true;  // Landed right where we left off.
    }

    if (!tolerateCappedRepositioning) {
        // The cursor has been repositioned as it was sitting on a document that has been
        // removed by capped collection deletion. It is important that we error out in this case
        // so that consumers don't silently get 'holes' when scanning capped collections.
        // We don't make this guarantee for normal collections or for write operations like
        // capped TTL deletion so it is ok to skip ahead in that case.
        _eof = true;
        return false;
    }

    // With bounded cursors, we should always find a key greater than the one we searched for.
    dassert(_forward ? cmp > 0 : cmp < 0);

    // We landed after where we were. Return our new location on the next call to next().
    _skipNextAdvance = true;
    return true;
}

WiredTigerStandardCappedCursor::WiredTigerStandardCappedCursor(OperationContext* opCtx,
                                                               RecoveryUnit& ru,
                                                               const WiredTigerRecordStore& rs,
                                                               bool forward)
    : WiredTigerCappedCursorBase(opCtx, ru, rs, forward) {
    init();
    initVisibility();
}

WiredTigerOplogCursor::WiredTigerOplogCursor(OperationContext* opCtx,
                                             RecoveryUnit& ru,
                                             const WiredTigerRecordStore& rs,
                                             bool forward)
    : WiredTigerCappedCursorBase(opCtx, ru, rs, forward) {
    init();
    initVisibility();
}

void WiredTigerOplogCursor::initVisibility() {
    auto& wtRu = WiredTigerRecoveryUnit::get(*_ru);
    if (_forward) {
        _oplogVisibleTs = wtRu.getOplogVisibilityTs();
    }
    boost::optional<Timestamp> readTs = wtRu.getPointInTimeReadTimestamp();
    if (readTs && readTs->asLL() != 0) {
        // One cannot pass a read_timestamp of 0 to WT, but a "0" is commonly understood as
        // every time is visible.
        _readTimestampForOplog = readTs->asInt64();
    }
}

bool WiredTigerOplogCursor::isVisible(const RecordId& id) {
    if (_readTimestampForOplog && id.getLong() > *_readTimestampForOplog) {
        return false;
    }
    if (!_forward) {
        return true;
    }
    if (_oplogVisibleTs && id.getLong() > *_oplogVisibleTs) {
        return false;
    }
    return true;
}

void WiredTigerOplogCursor::resetVisibility() {
    _oplogVisibleTs = boost::none;
    _readTimestampForOplog = boost::none;
}

boost::optional<Record> WiredTigerOplogCursor::next() {
    auto id = nextIdCommon();
    if (id.isNull()) {
        return boost::none;
    }

    auto cur = _cursor->get();

    // If we're using a read timestamp and we're a reverse cursor positioned outside of that bound,
    // walk backwards until we find a suitable record. This is exercised when doing a reverse
    // natural order collection scan.
    if (_readTimestampForOplog && !_forward) {
        while (id.getLong() > *_readTimestampForOplog) {
            int advanceRet = wiredTigerPrepareConflictRetry(
                *_opCtx,
                StorageExecutionContext::get(_opCtx)->getPrepareConflictTracker(),
                *_ru,
                [&] { return cur->prev(cur); });
            if (advanceRet == WT_NOTFOUND) {
                _positioned = false;
                _eof = true;
                return {};
            }
            invariantWTOK(advanceRet, cur->session);
            id = getKey(cur, _keyFormat);
        }
    }

    if (!isVisible(id)) {
        _eof = true;
        return {};
    }

    Record toReturn = {std::move(id), getRecordData(cur)};
    checkOrder(toReturn.id);
    trackReturn(toReturn);
    return toReturn;
}

boost::optional<Record> WiredTigerOplogCursor::seek(const RecordId& start,
                                                    BoundInclusion boundInclusion) {
    RecordId id;
    if (!_forward && _readTimestampForOplog && start.getLong() > *_readTimestampForOplog) {
        auto key = RecordId(*_readTimestampForOplog);
        id = seekIdCommon(key, BoundInclusion::kInclude);
    } else {
        id = seekIdCommon(start, boundInclusion);
    }

    if (id.isNull()) {
        return boost::none;
    }

    if (!isVisible(id)) {
        _eof = true;
        return boost::none;
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    trackReturn(toReturn);
    return toReturn;
}

}  // namespace mongo
