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

#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"

#include "mongo/base/string_data.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_compiled_configuration.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index_cursor_generic.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#define TRACING_ENABLED 0

#define LOGV2_TRACE_CURSOR(ID, NAME, ...)                  \
    if (TRACING_ENABLED)                                   \
    LOGV2(ID,                                              \
          "WT index ({index}) " #NAME,                     \
          "index"_attr = reinterpret_cast<uint64_t>(this), \
          "indexName"_attr = _indexName,                   \
          "uri"_attr = _uri,                               \
          "collectionUUID"_attr = _collectionUUID,         \
          ##__VA_ARGS__)

#define LOGV2_TRACE_INDEX(ID, NAME, ...)                   \
    if (TRACING_ENABLED)                                   \
    LOGV2(ID,                                              \
          "WT index ({index}) " #NAME,                     \
          "index"_attr = reinterpret_cast<uint64_t>(this), \
          ##__VA_ARGS__)

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(WTIndexUassertDuplicateRecordForKeyOnIdUnindex);
MONGO_FAIL_POINT_DEFINE(WTIndexCreateUniqueIndexesInOldFormat);
MONGO_FAIL_POINT_DEFINE(WTIndexInsertUniqueKeysInOldFormat);

static const WiredTigerItem emptyItem(nullptr, 0);

static CompiledConfiguration lowerInclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=lower,inclusive=true");
static CompiledConfiguration upperInclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=upper,inclusive=true");
static CompiledConfiguration clearBoundConfig("WT_CURSOR.bound", "action=clear");

/**
 * Returns the logv2::LogOptions controlling the behaviour after logging a data corruption error.
 * When the TestingProctor is enabled we will fatally assert. When the testing proctor is disabled
 * or when 'forceUassert' is specified (for instance because a failpoint is enabled), we should log
 * and throw DataCorruptionDetected.
 */
logv2::LogOptions getLogOptionsForDataCorruption(RecoveryUnit& ru, bool forceUassert = false) {
    if (ru.getDataCorruptionDetectionMode() == DataCorruptionDetectionMode::kThrow ||
        MONGO_unlikely(forceUassert)) {
        return logv2::LogOptions{logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)};
    } else {
        return logv2::LogOptions(logv2::LogComponent::kAutomaticDetermination);
    }
}

void setKey(WT_CURSOR* cursor, std::span<const char> value) {
    cursor->set_key(cursor, WiredTigerItem(value).get());
}

}  // namespace

void WiredTigerIndex::getKey(WT_CURSOR* cursor, WT_ITEM* key) {
    invariantWTOK(cursor->get_key(cursor, key), cursor->session);
}

// static
StatusWith<std::string> WiredTigerIndex::parseIndexOptions(const BSONObj& options) {
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

// static
std::string WiredTigerIndex::generateAppMetadataString(const IndexConfig& config) {
    int dataFormatVersion;
    bool isV2OrAbove = config.version >= IndexConfig::IndexVersion::kV2;

    if (config.unique && !config.isIdIndex) {
        if (MONGO_unlikely(WTIndexCreateUniqueIndexesInOldFormat.shouldFail())) {
            LOGV2(8596200,
                  "Creating unique index with old format version due to "
                  "WTIndexCreateUniqueIndexesInOldFormat failpoint",
                  "config"_attr = config.toString());
            dataFormatVersion = isV2OrAbove ? kDataFormatV4KeyStringV1UniqueIndexVersionV2
                                            : kDataFormatV3KeyStringV0UniqueIndexVersionV1;
        } else {
            dataFormatVersion = isV2OrAbove ? kDataFormatV6KeyStringV1UniqueIndexVersionV2
                                            : kDataFormatV5KeyStringV0UniqueIndexVersionV1;
        }
    } else {
        dataFormatVersion = isV2OrAbove ? kDataFormatV2KeyStringV1IndexVersionV2
                                        : kDataFormatV1KeyStringV0IndexVersionV1;
    }

    // Index metadata
    return fmt::format(",app_metadata=(formatVersion={}),", dataFormatVersion);
}

// static
StatusWith<std::string> WiredTigerIndex::generateCreateString(const std::string& engineName,
                                                              const std::string& sysIndexConfig,
                                                              const std::string& collIndexConfig,
                                                              StringData tableName,
                                                              const IndexConfig& config,
                                                              bool isLogged) {
    str::stream ss;

    // Separate out a prefix and suffix in the default string. User configuration will override
    // values in the prefix, but not values in the suffix.  Page sizes are chosen so that index
    // keys (up to 1024 bytes) will not overflow.
    ss << "type=file,internal_page_max=16k,leaf_page_max=16k,";
    ss << "checksum=on,";
    if (wiredTigerGlobalOptions.useIndexPrefixCompression) {
        ss << "prefix_compression=true,";
    }

    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig(tableName);
    ss << WiredTigerCustomizationHooksRegistry::get(getGlobalServiceContext())
              .getTableCreateConfig(tableName);
    ss << sysIndexConfig << ",";
    ss << collIndexConfig << ",";

    // Validate configuration object.
    // Raise an error about unrecognized fields that may be introduced in newer versions of
    // this storage engine.
    // Ensure that 'configString' field is a string. Raise an error if this is not the case.
    BSONElement storageEngineElement = config.infoObj["storageEngine"];
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
    ss << ",key_format=u";
    ss << ",value_format=u";

    // Index metadata
    ss << generateAppMetadataString(config);
    if (isLogged) {
        ss << "log=(enabled=true)";
    } else {
        ss << "log=(enabled=false)";
    }

    LOGV2_DEBUG(51779, 3, "index create string", "str"_attr = ss.ss.str());
    return StatusWith<std::string>(ss);
}

Status WiredTigerIndex::create(WiredTigerRecoveryUnit& ru,
                               const std::string& uri,
                               const std::string& config) {
    // Don't use the session from the recovery unit: create should not be used in a transaction
    WiredTigerSession session(ru.getConnection());
    LOGV2_DEBUG(
        51780, 1, "create uri: {uri} config: {config}", "uri"_attr = uri, "config"_attr = config);
    return wtRCToStatus(session.create(uri.c_str(), config.c_str()), session);
}

Status WiredTigerIndex::Drop(WiredTigerRecoveryUnit& ru, const std::string& uri) {
    WiredTigerSession session(ru.getConnection());
    return wtRCToStatus(session.drop(uri.c_str(), nullptr), session);
}

WiredTigerIndex::WiredTigerIndex(OperationContext* ctx,
                                 RecoveryUnit& ru,
                                 const std::string& uri,
                                 const UUID& collectionUUID,
                                 StringData ident,
                                 KeyFormat rsKeyFormat,
                                 const IndexConfig& config,
                                 bool isLogged)
    : SortedDataInterface(
          _handleVersionInfo(ctx, ru, uri, ident, config, isLogged), config.ordering, rsKeyFormat),
      _container(std::make_shared<Ident>(ident), uri, WiredTigerUtil::genTableId()),
      _collectionUUID(collectionUUID),
      _indexName(config.indexName),
      _isLogged(isLogged) {}

namespace {
void dassertRecordIdAtEnd(const key_string::View& keyString, KeyFormat keyFormat) {
    if (!kDebugBuild) {
        return;
    }

    RecordId rid = key_string::decodeRecordIdAtEnd(keyString.getKeyAndRecordIdView(), keyFormat);
    invariant(rid.isValid(), rid.toString());
}
}  // namespace

std::variant<Status, SortedDataInterface::DuplicateKey> WiredTigerIndex::insert(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const key_string::View& keyString,
    bool dupsAllowed,
    IncludeDuplicateRecordId includeDuplicateRecordId) {
    dassertRecordIdAtEnd(keyString, _rsKeyFormat);

    LOGV2_TRACE_INDEX(20093, "KeyString: {keyString}", "keyString"_attr = keyString);

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    auto cursorParams = getWiredTigerCursorParams(wtRu, _container.tableId());
    WiredTigerCursor curwrap(
        std::move(cursorParams), std::string{_container.uri()}, *wtRu.getSession());
    wtRu.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();

    return _insert(
        opCtx, ru, c, curwrap.getSession(), keyString, dupsAllowed, includeDuplicateRecordId);
}

void WiredTigerIndex::unindex(OperationContext* opCtx,
                              RecoveryUnit& ru,
                              const key_string::View& keyString,
                              bool dupsAllowed) {
    dassertRecordIdAtEnd(keyString, _rsKeyFormat);

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    auto cursorParams = getWiredTigerCursorParams(wtRu, _container.tableId());
    WiredTigerCursor curwrap(
        std::move(cursorParams), std::string{_container.uri()}, *wtRu.getSession());
    wtRu.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    _unindex(opCtx, ru, c, keyString, dupsAllowed);
}

boost::optional<RecordId> WiredTigerIndex::findLoc(OperationContext* opCtx,
                                                   RecoveryUnit& ru,
                                                   std::span<const char> key) const {
    auto cursor = newCursor(opCtx, ru);
    return cursor->seekExact(ru, key);
}

IndexValidateResults WiredTigerIndex::validate(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const CollectionValidation::ValidationOptions& options) const {
    IndexValidateResults results;
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    WiredTigerUtil::validateTableLogging(
        *wtRu.getSessionNoTxn(), _container.uri(), _isLogged, StringData{_indexName}, results);

    if (!options.isFullIndexValidation()) {
        invariant(!options.verifyConfigurationOverride().has_value());
        return results;
    }

    WiredTigerIndexUtil::validateStructure(
        wtRu, std::string{_container.uri()}, options.verifyConfigurationOverride(), results);

    return results;
}

int64_t WiredTigerIndex::numEntries(OperationContext* opCtx, RecoveryUnit& ru) const {
    int64_t count = 0;

    LOGV2_TRACE_INDEX(20094, "numEntries");

    auto keyInclusion =
        TRACING_ENABLED ? Cursor::KeyInclusion::kInclude : Cursor::KeyInclusion::kExclude;
    auto cursor = newCursor(opCtx, ru);
    for (auto kv = cursor->next(ru, keyInclusion); kv; kv = cursor->next(ru, keyInclusion)) {
        LOGV2_TRACE_INDEX(20095, "numEntries", "kv"_attr = kv);
        count++;
    }

    return count;
}

bool WiredTigerIndex::appendCustomStats(OperationContext* opCtx,
                                        RecoveryUnit& ru,
                                        BSONObjBuilder* output,
                                        double scale) const {
    return WiredTigerIndexUtil::appendCustomStats(
        WiredTigerRecoveryUnit::get(ru), output, scale, std::string{_container.uri()});
}

boost::optional<SortedDataInterface::DuplicateKey> WiredTigerIndex::dupKeyCheck(
    OperationContext* opCtx, RecoveryUnit& ru, const key_string::View& key) {
    invariant(unique());

    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    // Allow overwrite because it's faster and this is a read-only cursor.
    auto cursorParams =
        getWiredTigerCursorParams(wtRu, _container.tableId(), true /* allowOverwrite */);
    WiredTigerCursor curwrap(
        std::move(cursorParams), std::string{_container.uri()}, *wtRu.getSession());
    WT_CURSOR* c = curwrap.get();

    if (isDup(opCtx, ru, c, curwrap.getSession(), key)) {
        return DuplicateKey{key_string::toBson(key, _ordering)};
    }
    return boost::none;
}

bool WiredTigerIndex::isEmpty(OperationContext* opCtx, RecoveryUnit& ru) {
    return WiredTigerIndexUtil::isEmpty(opCtx,
                                        WiredTigerRecoveryUnit::get(ru),
                                        std::string{_container.uri()},
                                        _container.tableId());
}

void WiredTigerIndex::printIndexEntryMetadata(OperationContext* opCtx,
                                              RecoveryUnit& ru,
                                              const key_string::View& keyString) const {
    // Printing the index entry metadata requires a new session. We cannot open other cursors when
    // there are open history store cursors in the session. We also need to make sure that the
    // existing session has not written data to avoid potential deadlocks.
    invariant(!ru.inUnitOfWork());
    WiredTigerSession session(WiredTigerRecoveryUnit::get(ru).getConnection());

    // Per the version cursor API:
    // - A version cursor can only be called with the read timestamp as the oldest timestamp.
    // - If there is no oldest timestamp, the version cursor can only be called with a read
    //   timestamp of 1.
    // - If there is an oldest timestamp, reading at timestamp 1 will get rounded up.
    const std::string config = "read_timestamp=1,roundup_timestamps=(read=true)";
    WiredTigerBeginTxnBlock beginTxn(&session, config.c_str());

    // Open a version cursor. This is a debug cursor that enables iteration through the history of
    // values for a given index entry.
    WT_CURSOR* cursor =
        session.getNewCursor(std::string{_container.uri()}, "debug=(dump_version=(enabled=true))");

    setKey(cursor, keyString.getKeyAndRecordIdView());

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

        LOGV2(6601200,
              "WiredTiger index entry metadata",
              "keyString"_attr = keyString,
              "indexKey"_attr = key_string::toBson(keyString, _ordering),
              "startTxnId"_attr = startTxnId,
              "startTs"_attr = Timestamp(startTs),
              "startDurableTs"_attr = Timestamp(startDurableTs),
              "stopTxnId"_attr = stopTxnId,
              "stopTs"_attr = Timestamp(stopTs),
              "stopDurableTs"_attr = Timestamp(stopDurableTs),
              "type"_attr = type,
              "prepare"_attr = prepare,
              "flags"_attr = flags,
              "location"_attr = location);

        ret = cursor->next(cursor);
    }
}

long long WiredTigerIndex::getSpaceUsedBytes(OperationContext* opCtx, RecoveryUnit& ru) const {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    WiredTigerSession* s = wtRu.getSession();

    if (wtRu.getConnection()->isEphemeral()) {
        return static_cast<long long>(
            WiredTigerUtil::getEphemeralIdentSize(*s, std::string{_container.uri()}));
    }
    return static_cast<long long>(WiredTigerUtil::getIdentSize(*s, std::string{_container.uri()}));
}

long long WiredTigerIndex::getFreeStorageBytes(OperationContext* opCtx, RecoveryUnit& ru) const {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    WiredTigerSession* session = wtRu.getSessionNoTxn();

    return static_cast<long long>(
        WiredTigerUtil::getIdentReuseSize(*session, std::string{_container.uri()}));
}

Status WiredTigerIndex::initAsEmpty() {
    // No-op
    return Status::OK();
}

StatusWith<int64_t> WiredTigerIndex::compact(OperationContext* opCtx,
                                             RecoveryUnit& ru,
                                             const CompactOptions& options) {
    return WiredTigerIndexUtil::compact(
        opCtx, WiredTigerRecoveryUnit::get(ru), std::string{_container.uri()}, options);
}

Status WiredTigerIndex::truncate(OperationContext* opCtx, RecoveryUnit& ru) {
    WiredTigerUtil::truncate(WiredTigerRecoveryUnit::get(ru), std::string{_container.uri()});
    return Status::OK();
}

StringKeyedContainer& WiredTigerIndex::getContainer() {
    return _container;
}

const StringKeyedContainer& WiredTigerIndex::getContainer() const {
    return _container;
}

WiredTigerStringKeyedContainer& WiredTigerIndex::getWiredTigerContainer() {
    return _container;
}

boost::optional<RecordId> WiredTigerIndex::_keyExists(OperationContext* opCtx,
                                                      RecoveryUnit& ru,
                                                      WT_CURSOR* c,
                                                      WiredTigerSession* session,
                                                      const key_string::View& keyString) {
    // Given a KeyString KS with RecordId RID appended to the end, set the:
    // 1. Lower bound (inclusive) to be KS without RID
    // 2. Upper bound (inclusive) to be
    //   a. KS with RecordId::maxLong() for KeyFormat::Long
    //   b. KS with RecordId(FF00) for KeyFormat::String
    //
    // For example, KS = "key" and RID = "ABC123". The lower bound is "key" and the upper bound is
    // "keyFF00".
    setKey(c, keyString.getKeyView());
    invariantWTOK(c->bound(c, lowerInclusiveBoundConfig.getConfig(session)), c->session);
    _setUpperBoundForKeyExists(c, session, keyString);
    ON_BLOCK_EXIT([c, session] {
        invariantWTOK(c->bound(c, clearBoundConfig.getConfig(session)), c->session);
    });

    // The cursor is bounded to a prefix. Doing a next on the un-positioned cursor will position on
    // the first key that is equal to or more than the prefix.
    int ret = wiredTigerPrepareConflictRetry(
        *opCtx, StorageExecutionContext::get(opCtx)->getPrepareConflictTracker(), ru, [&] {
            return c->next(c);
        });

    if (ret == WT_NOTFOUND)
        return boost::none;
    invariantWTOK(ret, c->session);

    WiredTigerItem key;
    getKey(c, key.get());
    if (key.size() == keyString.getKeyView().size()) {
        invariant(_rsKeyFormat == KeyFormat::Long);

        // The prefix key is in the index without a RecordId appended to the key, which means that
        // the RecordId is instead stored in the value.
        WiredTigerItem value;
        invariantWTOK(c->get_value(c, value.get()), c->session);

        return key_string::decodeRecordIdLong(value);
    }

    return key_string::decodeRecordIdAtEnd(key, _rsKeyFormat);
}

void WiredTigerIndex::_setUpperBoundForKeyExists(WT_CURSOR* c,
                                                 WiredTigerSession* session,
                                                 const key_string::View& keyString) {
    key_string::Builder builder(keyString.getVersion(), _ordering);
    builder.resetFromBuffer(keyString.getKeyView());
    builder.appendRecordId(record_id_helpers::maxRecordId(_rsKeyFormat));

    setKey(c, builder.getView());
    invariantWTOK(c->bound(c, upperInclusiveBoundConfig.getConfig(session)), c->session);
}

std::variant<bool, SortedDataInterface::DuplicateKey> WiredTigerIndex::_checkDups(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    WT_CURSOR* c,
    WiredTigerSession* session,
    const key_string::View& keyString,
    IncludeDuplicateRecordId includeDuplicateRecordId) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    // A prefix key is KeyString of index key. It is the component of the index entry that
    // should be unique.
    auto prefix = keyString.getKeyView();
    // First phase inserts the prefix key to prohibit concurrent insertions of same key
    auto ret = _container.insert(wtRu, *c, prefix, {});

    // An entry with prefix key already exists. This can happen only during rolling upgrade when
    // both timestamp unsafe and timestamp safe index format keys could be present.
    if (ret == WT_DUPLICATE_KEY) {
        return DuplicateKey{key_string::toBson(
            prefix, _ordering, keyString.getTypeBitsView(), keyString.getVersion())};
    }
    invariantWTOK(
        ret,
        c->session,
        fmt::format("WiredTigerIndex::_insert: insert: {}; uri: {}", _indexName, _container.uri()));

    // Remove the prefix key. Our entry will continue to conflict with any concurrent
    // transactions, but will not conflict with any transaction that begins after this
    // operation commits.
    ret = _container.remove(wtRu, *c, prefix);
    invariantWTOK(
        ret,
        c->session,
        fmt::format("WiredTigerIndex::_insert: remove: {}; uri: {}", _indexName, _container.uri()));

    // The second phase looks for the key to avoid insertion of a duplicate key. The range bounded
    // cursor API restricts the key range we search within. This makes the search significantly
    // faster.
    auto rid = _keyExists(opCtx, ru, c, session, keyString);
    if (!rid) {
        return false;
    } else if (*rid == key_string::decodeRecordIdAtEnd(keyString.getRecordIdView(), _rsKeyFormat)) {
        return true;
    }

    boost::optional<RecordId> foundRecordId = boost::none;
    if (rid && includeDuplicateRecordId == IncludeDuplicateRecordId::kOn) {
        foundRecordId = *rid;
    }

    return DuplicateKey{
        key_string::toBson(prefix, _ordering, keyString.getTypeBitsView(), keyString.getVersion()),
        std::move(foundRecordId)};
}

void WiredTigerIndex::_repairDataFormatVersion(OperationContext* opCtx,
                                               RecoveryUnit& ru,
                                               const std::string& uri,
                                               StringData ident,
                                               const IndexConfig& config) {
    auto indexVersion = config.version;
    auto isIndexVersion1 = indexVersion == IndexConfig::IndexVersion::kV1;
    auto isIndexVersion2 = indexVersion == IndexConfig::IndexVersion::kV2;
    auto isDataFormat6 = _dataFormatVersion == kDataFormatV1KeyStringV0IndexVersionV1;
    auto isDataFormat8 = _dataFormatVersion == kDataFormatV2KeyStringV1IndexVersionV2;
    auto isDataFormat13 = _dataFormatVersion == kDataFormatV5KeyStringV0UniqueIndexVersionV1;
    auto isDataFormat14 = _dataFormatVersion == kDataFormatV6KeyStringV1UniqueIndexVersionV2;
    // Only fixes the index data format when it could be from an edge case when converting the
    // uniqueness of the index. Specifically:
    // * The index is a secondary unique index, but the data format version is 6 (v1) or 8 (v2).
    // * The index is a non-unique index, but the data format version is 13 (v1) or 14 (v2).
    if ((!config.isIdIndex && config.unique &&
         ((isIndexVersion1 && isDataFormat6) || (isIndexVersion2 && isDataFormat8))) ||
        (!config.unique &&
         ((isIndexVersion1 && isDataFormat13) || (isIndexVersion2 && isDataFormat14)))) {
        auto engine = opCtx->getServiceContext()->getStorageEngine();
        engine->getEngine()->alterIdentMetadata(ru,
                                                ident,
                                                config,
                                                /* isForceUpdateMetadata */ false);
        auto prevVersion = _dataFormatVersion;
        // The updated data format is guaranteed to be within the supported version range.
        _dataFormatVersion = WiredTigerUtil::checkApplicationMetadataFormatVersion(
                                 *WiredTigerRecoveryUnit::get(ru).getSessionNoTxn(),
                                 uri,
                                 kMinimumIndexVersion,
                                 kMaximumIndexVersion)
                                 .getValue();
        LOGV2_WARNING(6818600,
                      "Fixing index metadata data format version",
                      logAttrs(_collectionUUID),
                      "indexName"_attr = config.indexName,
                      "prevVersion"_attr = prevVersion,
                      "newVersion"_attr = _dataFormatVersion);
    }
}

key_string::Version WiredTigerIndex::_handleVersionInfo(OperationContext* ctx,
                                                        RecoveryUnit& ru,
                                                        const std::string& uri,
                                                        StringData ident,
                                                        const IndexConfig& config,
                                                        bool isLogged) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    auto version = WiredTigerUtil::checkApplicationMetadataFormatVersion(
        *wtRu.getSessionNoTxn(), uri, kMinimumIndexVersion, kMaximumIndexVersion);
    if (version == ErrorCodes::UnsupportedFormat || version == ErrorCodes::FailedToParse) {
        Status versionStatus = version.getStatus();
        Status indexVersionStatus(ErrorCodes::UnsupportedFormat,
                                  str::stream()
                                      << versionStatus.reason() << " Index: {name: "
                                      << config.indexName << ", ns: " << _collectionUUID
                                      << "} - version either too old or too new for this mongod.");
        fassertFailedWithStatus(28579, indexVersionStatus);
    }
    uassertStatusOK(version);
    _dataFormatVersion = version.getValue();

    _repairDataFormatVersion(ctx, ru, uri, ident, config);

    if (!config.isIdIndex && config.unique &&
        (_dataFormatVersion < kDataFormatV3KeyStringV0UniqueIndexVersionV1 ||
         _dataFormatVersion > kDataFormatV6KeyStringV1UniqueIndexVersionV2)) {
        Status versionStatus(
            ErrorCodes::UnsupportedFormat,
            str::stream() << "Index: {name: " << config.indexName << ", ns: " << _collectionUUID
                          << "} has incompatible format version: " << _dataFormatVersion);
        fassertFailedWithStatusNoTrace(31179, versionStatus);
    }

    uassertStatusOK(WiredTigerUtil::setTableLogging(*wtRu.getSession(), uri, isLogged));

    /*
     * Index data format 6, 11, and 13 correspond to KeyString version V0 and data format 8, 12, and
     * 14 correspond to KeyString version V1.
     */
    return (_dataFormatVersion == kDataFormatV2KeyStringV1IndexVersionV2 ||
            _dataFormatVersion == kDataFormatV4KeyStringV1UniqueIndexVersionV2 ||
            _dataFormatVersion == kDataFormatV6KeyStringV1UniqueIndexVersionV2)
        ? key_string::Version::V1
        : key_string::Version::V0;
}

namespace {

/**
 * Base class for WiredTigerIndex bulk builders.
 *
 * Manages the bulk cursor used by bulk builders.
 */
class BulkBuilder : public SortedDataBuilderInterface {
public:
    BulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx, RecoveryUnit& ru)
        : _idx(idx),
          _opCtx(opCtx),
          _cursor(opCtx, *WiredTigerRecoveryUnit::get(ru).getSession(), std::string{idx->uri()}) {}

protected:
    void insert(RecoveryUnit& ru, std::span<const char> key, std::span<const char> value) {
        invariantWTOK(_idx->getWiredTigerContainer().insert(
                          WiredTigerRecoveryUnit::get(ru), *_cursor.get(), key, value),
                      _cursor->session);
    }

    WiredTigerIndex* _idx;
    OperationContext* const _opCtx;
    WiredTigerBulkLoadCursor _cursor;
};

/**
 * Bulk builds a non-id index.
 */
class StandardBulkBuilder : public BulkBuilder {
public:
    StandardBulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx, RecoveryUnit& ru)
        : BulkBuilder(idx, opCtx, ru) {
        invariant(!_idx->isIdIndex());
    }

    // Standard and unique indexes use the key with the record id appended as the key, and store the
    // typebits in the value
    void addKey(RecoveryUnit& ru, const key_string::View& keyString) override {
        dassertRecordIdAtEnd(keyString, _idx->rsKeyFormat());
        insert(ru, keyString.getKeyAndRecordIdView(), keyString.getTypeBitsView());
    }
};

/**
 * Bulk builds an id index.
 */
class IdBulkBuilder : public BulkBuilder {
public:
    IdBulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx, RecoveryUnit& ru)
        : BulkBuilder(idx, opCtx, ru) {
        invariant(_idx->isIdIndex());
    }

    // Id indexes use only the key as the key, and the record id plus typebits go in the value
    void addKey(RecoveryUnit& ru, const key_string::View& newKeyString) override {
        dassertRecordIdAtEnd(newKeyString, KeyFormat::Long);
        insert(ru, newKeyString.getKeyView(), newKeyString.getRecordIdAndTypeBitsView());
    }
};

/**
 * Implements the basic WT_CURSOR functionality used by both unique and standard indexes.
 */
class WiredTigerIndexCursorBase : public SortedDataInterface::Cursor,
                                  public WiredTigerIndexCursorGeneric {
public:
    WiredTigerIndexCursorBase(const WiredTigerIndex& idx,
                              OperationContext* opCtx,
                              RecoveryUnit& ru,
                              bool forward)
        : WiredTigerIndexCursorGeneric(opCtx, forward),
          _ordering(idx.getOrdering()),
          _version(idx.getKeyStringVersion()),
          _rsKeyFormat(idx.rsKeyFormat()),
          _uri(idx.uri()),
          _tableId(idx.tableId()),
          _unique(idx.unique()),
          _isIdIndex(idx.isIdIndex()),
          _hasOldUniqueIndexFormat(idx.hasOldFormatVersion()),
          _indexName(idx.indexName()),
          _collectionUUID(idx.getCollectionUUID()),
          _key(idx.getKeyStringVersion()) {
        auto& wtRu = WiredTigerRecoveryUnit::get(ru);
        // Allow overwrite because it's faster and this is a read-only cursor.
        auto cursorParams = getWiredTigerCursorParams(wtRu, _tableId, true /* allowOverwrite */);
        _cursor.emplace(std::move(cursorParams), _uri, *wtRu.getSession());
    }

    boost::optional<IndexKeyEntry> next(RecoveryUnit& ru, KeyInclusion keyInclusion) override {
        advanceNext(ru);
        return curr(keyInclusion);
    }

    boost::optional<KeyStringEntry> nextKeyString(RecoveryUnit& ru) override {
        advanceNext(ru);
        return getKeyStringEntry();
    }

    SortedDataKeyValueView nextKeyValueView(RecoveryUnit& ru) override {
        advanceNext(ru);
        return getKeyValueView();
    }

    void setEndPosition(const BSONObj& key, bool inclusive) override {
        LOGV2_TRACE_CURSOR(20098,
                           "setEndPosition inclusive: {inclusive} {key}",
                           "inclusive"_attr = inclusive,
                           "key"_attr = key);
        if (key.isEmpty()) {
            // This means scan to end of index.
            _endPosition.reset();
            return;
        }

        // NOTE: this uses the opposite rules as a normal seek because a forward scan should
        // end after the key if inclusive and before if exclusive.
        const auto discriminator = _forward == inclusive
            ? key_string::Discriminator::kExclusiveAfter
            : key_string::Discriminator::kExclusiveBefore;
        key_string::Builder builder(_version);
        builder.resetToKey(key, _ordering, discriminator);
        setEndPosition(builder.getValueCopy());
    }

    void setEndPosition(const key_string::Value& keyString) override {
        LOGV2_TRACE_CURSOR(20089, "setEndPosition: {key}", "key"_attr = keyString);

        if (keyString.isEmpty()) {
            // This means scan to end of index.
            _endPosition.reset();
            return;
        }

        auto newEndPosition = std::make_unique<key_string::Value>(keyString);
        _endPosition.swap(newEndPosition);
    }

    boost::optional<IndexKeyEntry> seek(
        RecoveryUnit& ru,
        std::span<const char> keyString,
        KeyInclusion keyInclusion = KeyInclusion::kInclude) override {
        seekForKeyStringInternal(ru, keyString);
        return curr(keyInclusion);
    }

    boost::optional<KeyStringEntry> seekForKeyString(RecoveryUnit& ru,
                                                     std::span<const char> keyString) override {
        seekForKeyStringInternal(ru, keyString);
        return getKeyStringEntry();
    }

    SortedDataKeyValueView seekForKeyValueView(RecoveryUnit& ru,
                                               std::span<const char> keyStringValue) override {
        seekForKeyStringInternal(ru, keyStringValue);
        return getKeyValueView();
    }

    boost::optional<RecordId> seekExact(RecoveryUnit& ru,
                                        std::span<const char> keyString) override {
        seekForKeyStringInternal(ru, keyString);
        if (_eof) {
            return boost::none;
        }

        if (matchesPositionedKey(keyString)) {
            return _id;
        }

        return boost::none;
    }

    void save() override {
        // Make a copy of the key buffer to be used by restore()
        if (!_eof) {
            copyKey();
        }
        WiredTigerIndexCursorGeneric::resetCursor();
        // Forget the key buffer when resetting the cursor.
        _kvView.reset();

        // Our saved position is wherever we were when we last called updatePosition().
        // Any partially completed repositions should not effect our saved position.
    }

    void saveUnpositioned() override {
        // No need to copy the key in save()
        _kvView.reset();
        save();
        _eof = true;
        // Should not call seek() in restore()
        if (!_key.isEmpty()) {
            _key.resetToEmpty();
        }
    }

    void restore(RecoveryUnit& ru) override {
        auto& wtRu = WiredTigerRecoveryUnit::get(ru);
        if (!_cursor) {
            // Allow overwrite because it's faster and this is a read-only cursor.
            auto cursorParams =
                getWiredTigerCursorParams(wtRu, _tableId, true /* allowOverwrite */);
            _cursor.emplace(std::move(cursorParams), _uri, *wtRu.getSession());
        }

        // Ensure an active session exists, so any restored cursors will bind to it
        invariant(wtRu.getSession() == _cursor->getSession());

        if (!_key.isEmpty()) {
            const WiredTigerItem searchKey(_key.getView());
            _eof = !seekWTCursorInternal(ru, searchKey);
            if (!_eof) {
                WiredTigerItem curKey;
                WT_CURSOR* c = _cursor->get();
                getKey(c, curKey.get());

                // cmp is zero when we land on the exact same key we were positioned on before,
                // and non-zero otherwise.
                int cmp = key_string::compare(curKey, searchKey);
                dassert(_forward ? cmp >= 0 : cmp <= 0);
                _lastMoveSkippedKey = _forward ? cmp > 0 : cmp < 0;
            } else {
                // By setting _lastMoveSkippedKey to true we avoid trying to advance the cursor
                // when we are actually at EOF.
                _lastMoveSkippedKey = true;
            }
            LOGV2_TRACE_CURSOR(20099,
                               "restore _lastMoveSkippedKey: {lastMoveSkippedKey}",
                               "lastMoveSkippedKey"_attr = _lastMoveSkippedKey);
        }
    }

    bool isRecordIdAtEndOfKeyString() const override {
        return _kvView.isRecordIdAtEndOfKeyString();
    }

    void detachFromOperationContext() override {
        WiredTigerIndexCursorGeneric::detachFromOperationContext();
    }
    void reattachToOperationContext(OperationContext* opCtx) override {
        WiredTigerIndexCursorGeneric::reattachToOperationContext(opCtx);
    }
    void setSaveStorageCursorOnDetachFromOperationContext(bool detach) override {
        WiredTigerIndexCursorGeneric::setSaveStorageCursorOnDetachFromOperationContext(detach);
    }

protected:
    bool matchesPositionedKey(std::span<const char> search) const {
        auto ks = _kvView.getKeyStringWithoutRecordIdView();
        return key_string::compare(search, ks) == 0;
    }

    void copyKey() {
        if (!_kvView.isEmpty()) {
            _key.resetFromBuffer(_kvView.getKeyStringOriginalView());
            _keySizeWithoutRecordId = _kvView.getKeyStringWithoutRecordIdView().size();
        }
    }

    boost::optional<IndexKeyEntry> curr(KeyInclusion keyInclusion) const {
        if (_eof)
            return {};

        BSONObj bson;
        if (TRACING_ENABLED || keyInclusion == KeyInclusion::kInclude) {
            auto ks = _kvView.getKeyStringWithoutRecordIdView();
            auto tb = _kvView.getTypeBitsView();
            bson = key_string::toBson(ks, _ordering, tb, _version);
            LOGV2_TRACE_CURSOR(20000, "returning {bson} {id}", "bson"_attr = bson, "id"_attr = _id);
        }

        return {{std::move(bson), _id}};
    }

    // Returns false on EOF and when true, positions the cursor on a key greater than or equal
    // to query, direction dependent.
    [[nodiscard]] bool seekWTCursor(RecoveryUnit& ru, std::span<const char> query) {
        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(ru).getSession();

        // We must unposition our cursor by resetting so that we can set new bounds.
        if (_cursor) {
            WiredTigerIndexCursorGeneric::resetCursor();
            // Forget the key buffer when resetting the cursor.
            _kvView.reset();
        }

        return seekWTCursorInternal(ru, query);
    }

    // Returns false on EOF and when true, positions the cursor on a key greater than or equal
    // to searchKey, direction dependent.
    [[nodiscard]] bool seekWTCursorInternal(RecoveryUnit& ru, std::span<const char> searchKey) {
        WT_CURSOR* cur = _cursor->get();
        auto session = _cursor->getSession();
        if (_endPosition) {
            // Early-return in the unlikely case that our lower bound and upper bound overlap, which
            // is not allowed by the WiredTiger API.
            int cmp = key_string::compare(searchKey, _endPosition->getView());
            if (MONGO_unlikely((_forward && cmp > 0) || (!_forward && cmp < 0))) {
                return false;
            }

            setKey(cur, _endPosition->getView());

            auto const& config = _forward ? upperInclusiveBoundConfig : lowerInclusiveBoundConfig;
            invariantWTOK(cur->bound(cur, config.getConfig(session)), cur->session);
        }

        // When seeking with cursors, WiredTiger will traverse over deleted keys until it finds
        // its first non-deleted key. This can make it costly to search for a key that we just
        // deleted if there are many deleted values (e.g. TTL deletes). Additionally, we never
        // want to see a key that comes logically before the last key we returned. Thus, we
        // improve performance by setting a bound to indicate to WiredTiger to only consider
        // returning keys that are relevant to us. The cursor bound is by default inclusive of
        // the key being searched for. This also prevents us from seeing prepared updates on
        // unrelated keys.
        setKey(cur, searchKey);

        auto const& config = _forward ? lowerInclusiveBoundConfig : upperInclusiveBoundConfig;
        invariantWTOK(cur->bound(cur, config.getConfig(session)), cur->session);

        // Our cursor can only move in one direction, so there's no need to clear the bound
        // after seeking. We also don't want to clear our bounds so that the end bound is
        // maintained.
        int ret = wiredTigerPrepareConflictRetry(
            *_opCtx, StorageExecutionContext::get(_opCtx)->getPrepareConflictTracker(), ru, [&] {
                return _forward ? cur->next(cur) : cur->prev(cur);
            });

        // Forget the key buffer when repositioning the cursor.
        _kvView.reset();

        if (ret == WT_NOTFOUND) {
            return false;
        }
        invariantWTOK(ret, cur->session);
        return true;
    }

    /**
     * This must be called after moving the cursor to update our cached position. It should not
     * be called after a restore that did not restore to original state since that does not
     * logically move the cursor until the following call to next().
     * Must not throw WriteConflictException, throwing a WriteConflictException will retry the
     * operation effectively skipping over this key.
     */
    virtual void updatePosition(RecoveryUnit& ru,
                                std::span<const char> newKey,
                                std::span<const char> newValue) {
        auto keyOnly = key_string::withoutRecordIdAtEnd(newKey, _rsKeyFormat, &_id);
        invariant(!_id.isNull());

        _kvView = {keyOnly, newKey.subspan(keyOnly.size()), newValue, _version, true, &_id};
    }

    void checkKeyIsOrdered(std::span<const char> newKey) {
        if (!kDebugBuild || _key.isEmpty())
            return;
        // In debug mode, let's ensure that our index keys are actually in order. We've had
        // issues in the past with our underlying cursors (WT-2307), but also with cursor
        // mis-use (SERVER-55658). This check can help us catch such things earlier rather than
        // later.
        const int cmp = key_string::compare(_key.getView(), newKey);
        bool outOfOrder = _forward ? cmp > 0 : cmp < 0;

        if (outOfOrder) {
            LOGV2_FATAL(51790,
                        "WTIndex::checkKeyIsOrdered: the new key is out of order with respect to "
                        "the previous key",
                        "newKey"_attr = redact(hexblob::encode(newKey.data(), newKey.size())),
                        "prevKey"_attr = redact(_key.toString()),
                        "isForwardCursor"_attr = _forward);
        }
    }


    void seekForKeyStringInternal(RecoveryUnit& ru, std::span<const char> keyString) {
        _eof = !seekWTCursor(ru, keyString);

        _lastMoveSkippedKey = false;
        _id = RecordId();

        if (_eof)
            return;

        WT_CURSOR* c = _cursor->get();
        WiredTigerItem item;
        WiredTigerItem value;
        getKeyValue(c, item.get(), value.get());

        updatePosition(ru, item, value);
    }

    void advanceNext(RecoveryUnit& ru) {
        // Advance on a cursor at the end is a no-op.
        if (_eof)
            return;

        // In debug build, make a copy of the key buffer before advancing the cursor.
        // This supports checkKeyIsOrdered() which ensures WT returns keys in-order.
        if (kDebugBuild) {
            copyKey();
        }

        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(ru).getSession();

        if (!_lastMoveSkippedKey)
            _eof = !advanceWTCursor(ru);

        _lastMoveSkippedKey = false;

        if (_eof) {
            // In the normal case, _id will be updated in updatePosition. Making this reset
            // unconditional affects performance noticeably.
            _id = RecordId();
            return;
        }

        WT_CURSOR* c = _cursor->get();
        WiredTigerItem item;
        WiredTigerItem value;
        getKeyValue(c, item.get(), value.get());

        checkKeyIsOrdered(item);

        updatePosition(ru, item, value);
    }

    boost::optional<KeyStringEntry> getKeyStringEntry() {
        if (_eof)
            return {};
        auto key = _kvView.getValueCopy();
        LOGV2_TRACE_CURSOR(20091, "returning {key} {id}", "key"_attr = key, "id"_attr = _id);
        return KeyStringEntry(key, _id);
    }

    SortedDataKeyValueView getKeyValueView() {
        if (_eof)
            return {};
        LOGV2_TRACE_CURSOR(8615100, "returning {kvView}", "kvView"_attr = _kvView);
        return _kvView;
    }

    const Ordering _ordering;
    const key_string::Version _version;
    const KeyFormat _rsKeyFormat;
    const std::string _uri;
    const uint64_t _tableId;
    const bool _unique;
    const bool _isIdIndex;
    const bool _hasOldUniqueIndexFormat;
    const std::string _indexName;
    const UUID _collectionUUID;

    // These are where this cursor instance is. They are not changed in the face of a failing
    // next().
    SortedDataKeyValueView _kvView;
    RecordId _id;
    // These are owned copy of _kvView, made before the unowned data referenced by _kvView was
    // invalidated upon calling a next() variant or a save().
    key_string::Builder _key;
    int32_t _keySizeWithoutRecordId = 0;

    std::unique_ptr<key_string::Value> _endPosition;

    // Used by next to decide to return current position rather than moving. Should be reset to
    // false by any operation that moves the cursor, other than subsequent save/restore pairs.
    bool _lastMoveSkippedKey = false;

    bool _eof = false;
};
}  // namespace

// The Standard Cursor doesn't need anything more than the base has.
using WiredTigerIndexStandardCursor = WiredTigerIndexCursorBase;

class WiredTigerIndexUniqueCursor final : public WiredTigerIndexCursorBase {
public:
    WiredTigerIndexUniqueCursor(const WiredTigerIndex& idx,
                                OperationContext* opCtx,
                                RecoveryUnit& ru,
                                bool forward)
        : WiredTigerIndexCursorBase(idx, opCtx, ru, forward) {}

    void updatePosition(RecoveryUnit& ru,
                        std::span<const char> newKey,
                        std::span<const char> newValue) override {
        // After a rolling upgrade an index can have keys from both timestamp unsafe (old) and
        // timestamp safe (new) unique indexes. Detect correct index key format by checking key's
        // size. Old format keys just had the index key while new format key has index key + Record
        // id. _id indexes remain at the old format. When KeyString contains just the key, the
        // RecordId is in value.
        bool isRecordIdAtEndOfKeyString = true;
        if (_hasOldUniqueIndexFormat) {
            auto keySize = key_string::getKeySize(newKey, _ordering, _version);
            if (keySize == newKey.size()) {
                isRecordIdAtEndOfKeyString = false;
                // Old-format unique index keys always use the Long format.
                invariant(_rsKeyFormat == KeyFormat::Long);

                BufReader br(newValue.data(), newValue.size());
                _id = key_string::decodeRecordIdLong(&br);
                invariant(!_id.isNull());

                std::span typeBits(static_cast<const char*>(br.pos()), br.remaining());
                _kvView = {newKey,
                           newValue.first(newValue.size() - typeBits.size()),
                           typeBits,
                           _version,
                           false, /* isRecordIdAtEndOfKeyString */
                           &_id};

                // Check validity of the remaining buffer as TypeBits.
                key_string::TypeBits::getReaderFromBuffer(_version, &br);
                if (!br.atEof()) {
                    const auto bsonKey = redact(curr(KeyInclusion::kInclude)->key);
                    LOGV2_ERROR_OPTIONS(
                        7623202,
                        getLogOptionsForDataCorruption(ru),
                        "Unique index cursor seeing multiple records for key in index",
                        "key"_attr = bsonKey,
                        "index"_attr = _indexName,
                        "uri"_attr = _uri,
                        logAttrs(_collectionUUID));
                }
            }
        }
        if (isRecordIdAtEndOfKeyString) {
            // The RecordId is in the key at the end. This implementation is provided by the
            // base class, let us just invoke that functionality here.
            WiredTigerIndexCursorBase::updatePosition(ru, newKey, newValue);
        }

        auto ks = _kvView.getKeyStringOriginalView();
        LOGV2_TRACE_INDEX(20096,
                          "Unique Index KeyString: [{keyString}]",
                          "keyString"_attr = hexblob::encode(ks.data(), ks.size()));
    }

    void restore(RecoveryUnit& ru) override {
        // Lets begin by calling the base implementation
        WiredTigerIndexCursorBase::restore(ru);

        if (_lastMoveSkippedKey && !_eof) {
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

            // This check is only to avoid returning the same key again after a restore. Keys
            // shorter than _key cannot have "prefix key" same as _key. Therefore we care only about
            // the keys with size greater than or equal to that of the _key.
            invariant(!_key.isEmpty());
            invariant(_keySizeWithoutRecordId > 0);
            if (static_cast<int32_t>(item.size) >= _keySizeWithoutRecordId &&
                std::memcmp(_key.getView().data(), item.data, _keySizeWithoutRecordId) == 0) {
                _lastMoveSkippedKey = false;
                LOGV2_TRACE_CURSOR(20092, "restore _lastMoveSkippedKey changed to false.");
            }
        }
    }
};

class WiredTigerIdIndexCursor final : public WiredTigerIndexCursorBase {
public:
    WiredTigerIdIndexCursor(const WiredTigerIndex& idx,
                            OperationContext* opCtx,
                            RecoveryUnit& ru,
                            bool forward)
        : WiredTigerIndexCursorBase(idx, opCtx, ru, forward) {}

    // Must not throw WriteConflictException, throwing a WriteConflictException will retry the
    // operation effectively skipping over this key.
    void updatePosition(RecoveryUnit& ru,
                        std::span<const char> newKey,
                        std::span<const char> newValue) override {
        // _id index keys always use the Long format.
        invariant(_rsKeyFormat == KeyFormat::Long);

        BufReader br(newValue.data(), newValue.size());
        _id = key_string::decodeRecordIdLong(&br);
        invariant(!_id.isNull());
        auto ridView = newValue.first(br.offset());

        _kvView = {newKey,
                   ridView,
                   std::span(static_cast<const char*>(br.pos()), br.remaining()),
                   _version,
                   false, /* isRecordIdAtEndOfKeyString */
                   &_id};

        // Check validity of the remaining buffer as TypeBits.
        key_string::TypeBits::getReaderFromBuffer(_version, &br);

        if (!br.atEof()) {
            const auto bsonKey = redact(curr(KeyInclusion::kInclude)->key);

            LOGV2_ERROR_OPTIONS(5176200,
                                getLogOptionsForDataCorruption(ru),
                                "Index cursor seeing multiple records for key in _id index",
                                "key"_attr = bsonKey,
                                "index"_attr = _indexName,
                                "uri"_attr = _uri,
                                logAttrs(_collectionUUID));
        }
    }
};
//}  // namespace

WiredTigerIndexUnique::WiredTigerIndexUnique(OperationContext* ctx,
                                             RecoveryUnit& ru,
                                             const std::string& uri,
                                             const UUID& collectionUUID,
                                             StringData ident,
                                             KeyFormat rsKeyFormat,
                                             const IndexConfig& config,
                                             bool isLogged)
    : WiredTigerIndex(ctx, ru, uri, collectionUUID, ident, rsKeyFormat, config, isLogged) {
    // _id indexes must use WiredTigerIdIndex
    invariant(!isIdIndex());
    // All unique indexes should be in the timestamp-safe format version as of version 4.2.
    invariant(isTimestampSafeUniqueIdx());
}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIndexUnique::newCursor(
    OperationContext* opCtx, RecoveryUnit& ru, bool forward) const {
    return std::make_unique<WiredTigerIndexUniqueCursor>(*this, opCtx, ru, forward);
}

std::unique_ptr<SortedDataBuilderInterface> WiredTigerIndexUnique::makeBulkBuilder(
    OperationContext* opCtx, RecoveryUnit& ru) {
    return std::make_unique<StandardBulkBuilder>(this, opCtx, ru);
}

bool WiredTigerIndexUnique::isTimestampSafeUniqueIdx() const {
    if (_dataFormatVersion == kDataFormatV1KeyStringV0IndexVersionV1 ||
        _dataFormatVersion == kDataFormatV2KeyStringV1IndexVersionV2) {
        return false;
    }
    return true;
}

bool WiredTigerIndexUnique::isDup(OperationContext* opCtx,
                                  RecoveryUnit& ru,
                                  WT_CURSOR* c,
                                  WiredTigerSession* session,
                                  const key_string::View& prefixKey) {
    std::span prefix = prefixKey.getKeyView();

    // This procedure to determine duplicates is exclusive for timestamp safe unique indexes.
    // Check if a prefix key already exists in the index. When keyExists() returns true, the cursor
    // will be positioned on the first occurrence of the 'prefixKey'.
    if (!_keyExists(opCtx, ru, c, session, prefixKey)) {
        return false;
    }

    // If the next key also matches, this is a duplicate.
    int ret = wiredTigerPrepareConflictRetry(
        *opCtx, StorageExecutionContext::get(opCtx)->getPrepareConflictTracker(), ru, [&] {
            return c->next(c);
        });

    WT_ITEM item;
    if (ret == 0) {
        getKey(c, &item);
        return std::memcmp(prefix.data(), item.data, std::min(prefix.size(), item.size)) == 0;
    }

    // Make sure that next call did not fail due to any other error but not found. In case of
    // another error, we are not good to move forward.
    if (ret == WT_NOTFOUND) {
        return false;
    }

    fassertFailedWithStatus(40685, wtRCToStatus(ret, c->session));
    MONGO_UNREACHABLE;
}

WiredTigerIdIndex::WiredTigerIdIndex(OperationContext* ctx,
                                     RecoveryUnit& ru,
                                     const std::string& uri,
                                     const UUID& collectionUUID,
                                     StringData ident,
                                     const IndexConfig& config,
                                     bool isLogged)
    : WiredTigerIndex(ctx, ru, uri, collectionUUID, ident, KeyFormat::Long, config, isLogged) {
    invariant(isIdIndex());
}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIdIndex::newCursor(OperationContext* opCtx,
                                                                          RecoveryUnit& ru,
                                                                          bool forward) const {
    return std::make_unique<WiredTigerIdIndexCursor>(*this, opCtx, ru, forward);
}

std::unique_ptr<SortedDataBuilderInterface> WiredTigerIdIndex::makeBulkBuilder(
    OperationContext* opCtx, RecoveryUnit& ru) {
    return std::make_unique<IdBulkBuilder>(this, opCtx, ru);
}

std::variant<Status, SortedDataInterface::DuplicateKey> WiredTigerIdIndex::_insert(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    WT_CURSOR* c,
    WiredTigerSession* session,
    const key_string::View& keyString,
    bool dupsAllowed,
    IncludeDuplicateRecordId includeDuplicateRecordId) {
    invariant(KeyFormat::Long == _rsKeyFormat);
    invariant(!dupsAllowed);
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    int ret =
        _container.insert(wtRu, *c, keyString.getKeyView(), keyString.getRecordIdAndTypeBitsView());
    if (ret != WT_DUPLICATE_KEY) {
        return wtRCToStatus(ret, c->session, [this]() {
            return fmt::format(
                "WiredTigerIdIndex::_insert: index: {}; uri: {}", _indexName, _container.uri());
        });
    }

    DuplicateKeyErrorInfo::FoundValue foundValueRecordId;
    boost::optional<RecordId> duplicateRecordId;
    if (TestingProctor::instance().isEnabled() ||
        includeDuplicateRecordId == IncludeDuplicateRecordId::kOn) {
        WT_ITEM foundValue;
        invariantWTOK(c->get_value(c, &foundValue), c->session);

        BufReader reader(foundValue.data, foundValue.size);
        duplicateRecordId = key_string::decodeRecordIdLong(&reader);
        foundValueRecordId = *duplicateRecordId;
    }

    return DuplicateKey{key_string::toBson(keyString, _ordering),
                        std::move(duplicateRecordId),
                        std::move(foundValueRecordId)};
}

std::variant<Status, SortedDataInterface::DuplicateKey> WiredTigerIndexUnique::_insert(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    WT_CURSOR* c,
    WiredTigerSession* session,
    const key_string::View& keyString,
    bool dupsAllowed,
    IncludeDuplicateRecordId includeDuplicateRecordId) {
    LOGV2_TRACE_INDEX(
        20097, "Timestamp safe unique idx KeyString: {keyString}", "keyString"_attr = keyString);
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    // Pre-checks before inserting on a primary.
    if (!dupsAllowed) {
        auto result = _checkDups(opCtx, ru, c, session, keyString, includeDuplicateRecordId);
        if (auto* duplicate = std::get_if<DuplicateKey>(&result)) {
            return *duplicate;
        } else if (std::get<bool>(result)) {
            return Status::OK();
        }
    }

    // Prior to v4.2 unique indexes put the record id in the value field on the index entry rather
    // than appending it to the key. Existing indexes are not rewritten when upgrading versions, so
    // we need to be able to insert keys in the old format for testing purposes. Given the required
    // prerequisites, the WTIndexCreateUniqueIndexesInOldFormat failpoint can be set to insert keys
    // in the old format. We can't do this when dups are allowed (on secondaries), as this could
    // result in concurrent writes to the same key, the very thing the new format intends to avoid.
    // Old format keys also predated KeyFormat::String, so those cannot be inserted.
    bool forceOldFormat = !dupsAllowed && hasOldFormatVersion() &&
        _rsKeyFormat == KeyFormat::Long && WTIndexInsertUniqueKeysInOldFormat.shouldFail();
    auto ret = [&] {
        if (MONGO_unlikely(forceOldFormat)) {
            LOGV2_DEBUG(8596201,
                        1,
                        "Inserting old format key into unique index due to "
                        "WTIndexInsertUniqueKeysInOldFormat failpoint",
                        "key"_attr = keyString.toString(),
                        "indexName"_attr = _indexName,
                        "collectionUUID"_attr = _collectionUUID);
            return _container.insert(
                wtRu, *c, keyString.getKeyView(), keyString.getRecordIdAndTypeBitsView());
        }
        return _container.insert(
            wtRu, *c, keyString.getKeyAndRecordIdView(), keyString.getTypeBitsView());
    }();

    // Unless we're forcing the old format, It is possible that this key is already present during a
    // concurrent background index build.
    if (ret != WT_DUPLICATE_KEY || forceOldFormat) {
        invariantWTOK(ret,
                      c->session,
                      fmt::format("WiredTigerIndexUnique::_insert: duplicate: {}; uri: {}",
                                  _indexName,
                                  _container.uri()));
    }

    return Status::OK();
}

void WiredTigerIdIndex::_unindex(OperationContext* opCtx,
                                 RecoveryUnit& ru,
                                 WT_CURSOR* c,
                                 const key_string::View& keyString,
                                 bool dupsAllowed) {
    invariant(KeyFormat::Long == _rsKeyFormat);
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    const auto failWithDataCorruptionForTest =
        WTIndexUassertDuplicateRecordForKeyOnIdUnindex.shouldFail();
    // On the _id index, the RecordId is stored in the value of the index entry. If the dupsAllowed
    // flag is not set, we blindly delete using only the key without checking the RecordId.
    if (!dupsAllowed && MONGO_likely(!failWithDataCorruptionForTest)) {
        int ret = WT_OP_CHECK(_container.remove(wtRu, *c, keyString.getKeyView()));
        if (ret == WT_NOTFOUND) {
            return;
        }
        invariantWTOK(ret, c->session);
        return;
    }

    // Duplicates are never actually allowed on _id indexes, however the 'dupsAllowed' convention
    // requires that we check the value of the RecordId in the keyString instead of blindly deleting
    // based on just the first part of the key.
    setKey(c, keyString.getKeyView());
    int ret = wiredTigerPrepareConflictRetry(
        *opCtx, StorageExecutionContext::get(opCtx)->getPrepareConflictTracker(), ru, [&] {
            return c->search(c);
        });
    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret, c->session);

    WT_ITEM old;
    invariantWTOK(c->get_value(c, &old), c->session);

    BufReader br(old.data, old.size);
    invariant(br.remaining());

    RecordId idInIndex = key_string::decodeRecordIdLong(&br);
    key_string::TypeBits typeBits = key_string::TypeBits::fromBuffer(getKeyStringVersion(), &br);
    if (!br.atEof() || MONGO_unlikely(failWithDataCorruptionForTest)) {
        auto bsonKey = key_string::toBson(keyString, _ordering);

        LOGV2_ERROR_OPTIONS(5176201,
                            getLogOptionsForDataCorruption(ru, failWithDataCorruptionForTest),
                            "Un-index seeing multiple records for key",
                            "key"_attr = bsonKey,
                            "index"_attr = _indexName,
                            "uri"_attr = _container.uri(),
                            logAttrs(_collectionUUID));
    }

    auto id = key_string::decodeRecordIdLongAtEnd(keyString.getRecordIdView());
    invariant(id.isValid());

    // The RecordId matches, so remove the entry.
    if (id == idInIndex) {
        invariantWTOK(WT_OP_CHECK(wiredTigerCursorRemove(wtRu, c)), c->session);
        return;
    }

    auto key = key_string::toBson(keyString, _ordering);
    LOGV2_WARNING(51797,
                  "Associated record not found in collection while removing index entry",
                  logAttrs(_collectionUUID),
                  "index"_attr = _indexName,
                  "key"_attr = redact(key),
                  "recordId"_attr = id);
}

void WiredTigerIndexUnique::_unindex(OperationContext* opCtx,
                                     RecoveryUnit& ru,
                                     WT_CURSOR* c,
                                     const key_string::View& keyString,
                                     bool dupsAllowed) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    // Note that the dupsAllowed flag asks us to check that the RecordId in the KeyString matches
    // before deleting any keys. Unique indexes store RecordIds in the keyString, so we get this
    // behavior by default.
    int ret = WT_OP_CHECK(_container.remove(wtRu, *c, keyString.getKeyAndRecordIdView()));

    if (ret != WT_NOTFOUND) {
        invariantWTOK(ret, c->session);
        return;
    }

    if (KeyFormat::String == _rsKeyFormat) {
        // This is a unique index on a clustered collection. These indexes will only have keys
        // in the timestamp safe format where the RecordId is appended at the end of the key.
        return;
    }

    // WT_NOTFOUND is possible if index key is in old (v4.0) format. Retry removal of key using old
    // format.
    if (hasOldFormatVersion()) {
        _unindexTimestampUnsafe(opCtx, ru, c, keyString, dupsAllowed);
    }
}

void WiredTigerIndexUnique::_unindexTimestampUnsafe(OperationContext* opCtx,
                                                    RecoveryUnit& ru,
                                                    WT_CURSOR* c,
                                                    const key_string::View& keyString,
                                                    bool dupsAllowed) {
    // The old unique index format had a key-value of indexKey-RecordId. This means that the
    // RecordId in an index entry might not match the indexKey+RecordId keyString passed into this
    // function. There is at least one case where this is required: an index on a field where
    // multiple collection documents have the same field value but only one passes the partial index
    // filter. However, we can't make assumptions about why callers would try to remove a key with a
    // mismatched RecordId. If they try, we must ensure we always delete the correct key.
    //
    // The dupsAllowed flag is no longer relevant for the old unique index format. No new index
    // entries are written in the old format, let alone during temporary phases of the server when
    // duplicates are allowed.

    setKey(c, keyString.getKeyView());

    int ret = wiredTigerPrepareConflictRetry(
        *opCtx, StorageExecutionContext::get(opCtx)->getPrepareConflictTracker(), ru, [&] {
            return c->search(c);
        });
    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret, c->session);

    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value), c->session);
    BufReader br(value.data, value.size);
    fassert(40416, br.remaining());

    // Check that the record id matches. We may be called to unindex records that are not present in
    // the index. We must always ensure we do not delete a key with a different RecordId.
    RecordId id = key_string::decodeRecordIdLongAtEnd(keyString.getRecordIdView());
    invariant(id.isValid());
    bool foundRecord = key_string::decodeRecordIdLong(&br) == id;

    // Ensure the index entry value is not a list of RecordIds, which should only be possible
    // temporarily in v4.0 when dupsAllowed is true, not ever across upgrades or in upgraded
    // versions.
    key_string::TypeBits::fromBuffer(getKeyStringVersion(), &br);
    if (br.remaining()) {
        LOGV2_FATAL_NOTRACE(
            7592201,
            "An index entry was found that contains an unexpected old format that should no "
            "longer exist. The index should be dropped and rebuilt.",
            "indexName"_attr = _indexName,
            "collectionUUID"_attr = _collectionUUID);
    }

    if (!foundRecord) {
        return;
    }

    ret = WT_OP_CHECK(wiredTigerCursorRemove(WiredTigerRecoveryUnit::get(ru), c));
    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret, c->session);
}
// ------------------------------

WiredTigerIndexStandard::WiredTigerIndexStandard(OperationContext* ctx,
                                                 RecoveryUnit& ru,
                                                 const std::string& uri,
                                                 const UUID& collectionUUID,
                                                 StringData ident,
                                                 KeyFormat rsKeyFormat,
                                                 const IndexConfig& config,
                                                 bool isLogged)
    : WiredTigerIndex(ctx, ru, uri, collectionUUID, ident, rsKeyFormat, config, isLogged) {}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIndexStandard::newCursor(
    OperationContext* opCtx, RecoveryUnit& ru, bool forward) const {
    return std::make_unique<WiredTigerIndexStandardCursor>(*this, opCtx, ru, forward);
}

std::unique_ptr<SortedDataBuilderInterface> WiredTigerIndexStandard::makeBulkBuilder(
    OperationContext* opCtx, RecoveryUnit& ru) {
    return std::make_unique<StandardBulkBuilder>(this, opCtx, ru);
}

std::variant<Status, SortedDataInterface::DuplicateKey> WiredTigerIndexStandard::_insert(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    WT_CURSOR* c,
    WiredTigerSession* session,
    const key_string::View& keyString,
    bool dupsAllowed,
    IncludeDuplicateRecordId includeDuplicateRecordId) {
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);

    // Pre-checks before inserting on a primary.
    if (!dupsAllowed) {
        auto result = _checkDups(opCtx, ru, c, session, keyString, includeDuplicateRecordId);
        if (auto* duplicate = std::get_if<DuplicateKey>(&result)) {
            return *duplicate;
        } else if (std::get<bool>(result)) {
            return Status::OK();
        }
    }

    auto ret =
        _container.insert(wtRu, *c, keyString.getKeyAndRecordIdView(), keyString.getTypeBitsView());

    // If the record was already in the index, we return OK. This can happen, for example, when
    // building a background index while documents are being written and reindexed.
    if (!ret || ret == WT_DUPLICATE_KEY) {
        return Status::OK();
    }

    return wtRCToStatus(ret, c->session, [this]() {
        return fmt::format(
            "WiredTigerIndexStandard::_insert: index: {}; uri: {}", _indexName, _container.uri());
    });
}

void WiredTigerIndexStandard::_unindex(OperationContext* opCtx,
                                       RecoveryUnit& ru,
                                       WT_CURSOR* c,
                                       const key_string::View& keyString,
                                       bool dupsAllowed) {
    invariant(dupsAllowed);
    auto& wtRu = WiredTigerRecoveryUnit::get(ru);
    int ret = WT_OP_CHECK(_container.remove(wtRu, *c, keyString.getKeyAndRecordIdView()));

    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret, c->session);
}

}  // namespace mongo
