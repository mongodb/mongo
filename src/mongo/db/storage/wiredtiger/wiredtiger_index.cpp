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
#include "mongo/db/catalog/health_log.h"
#include "mongo/db/catalog/health_log_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index_cursor_generic.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/util/stacktrace.h"
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

MONGO_FAIL_POINT_DEFINE(WTIndexUassertDuplicateRecordForIdIndex);
MONGO_FAIL_POINT_DEFINE(WTIndexUassertDuplicateRecordForKeyOnIdUnindex);
MONGO_FAIL_POINT_DEFINE(WTIndexCreateUniqueIndexesInOldFormat);
MONGO_FAIL_POINT_DEFINE(WTIndexInsertUniqueKeysInOldFormat);

static const WiredTigerItem emptyItem(nullptr, 0);

/**
 * Add a data corruption entry to the health log.
 */
void addDataCorruptionEntryToHealthLog(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       StringData operation,
                                       StringData message,
                                       const BSONObj& key,
                                       StringData indexName,
                                       StringData uri) {
    HealthLogEntry entry;
    entry.setNss(nss);
    entry.setTimestamp(Date_t::now());
    entry.setSeverity(SeverityEnum::Error);
    entry.setScope(ScopeEnum::Index);
    entry.setOperation(operation);
    entry.setMsg(message);

    BSONObjBuilder bob;
    bob.append("key", key);
    bob.append("indexName", indexName);
    bob.append("uri", uri);
    bob.appendElements(getStackTrace().getBSONRepresentation());
    entry.setData(bob.obj());

    HealthLog::get(opCtx)->log(entry);
}

/**
 * Returns the logv2::LogOptions controlling the behaviour after logging a data corruption error.
 * When the TestingProctor is enabled we will fatally assert. When the testing proctor is disabled
 * or when 'forceUassert' is specified (for instance because a failpoint is enabled), we should log
 * and throw DataCorruptionDetected.
 */
logv2::LogOptions getLogOptionsForDataCorruption(bool forceUassert = false) {
    if (!TestingProctor::instance().isEnabled() || forceUassert) {
        return logv2::LogOptions{logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)};
    } else {
        return logv2::LogOptions(logv2::LogComponent::kAutomaticDetermination);
    }
}

/**
 * Compares two binary blobs lexicographically. Returns 0 on exact equality, a positive value when
 * bufA compares greater than bufB, and a negative value when bufA compares less than bufB. If both
 * blobs match up to a common prefix, the longer string is considered greater.
 */
int lexCompare(const void* bufA, int sizeA, const void* bufB, int sizeB) {
    int cmp = std::memcmp(bufA, bufB, std::min(sizeA, sizeB));
    if (cmp != 0) {
        return cmp;
    }
    return (sizeA == sizeB) ? 0 : (sizeA < sizeB) ? -1 : 1;
}
}  // namespace

void WiredTigerIndex::setKey(WT_CURSOR* cursor, const WT_ITEM* item) {
    cursor->set_key(cursor, item);
}

void WiredTigerIndex::getKey(WT_CURSOR* cursor,
                             WT_ITEM* key,
                             ResourceConsumption::MetricsCollector* metrics) {
    invariantWTOK(cursor->get_key(cursor, key), cursor->session);

    if (metrics) {
        metrics->incrementOneIdxEntryRead(_uri, key->size);
    }
}

// static
StatusWith<std::string> WiredTigerIndex::parseIndexOptions(const BSONObj& options) {
    StringBuilder ss;
    BSONForEach(elem, options) {
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
std::string WiredTigerIndex::generateAppMetadataString(const IndexDescriptor& desc) {
    int dataFormatVersion;

    if (desc.unique() && !desc.isIdIndex()) {
        if (MONGO_unlikely(WTIndexCreateUniqueIndexesInOldFormat.shouldFail())) {
            LOGV2(8596200,
                  "Creating unique index with old format version due to "
                  "WTIndexCreateUniqueIndexesInOldFormat failpoint",
                  "desc"_attr = desc.toString());
            dataFormatVersion = desc.version() >= IndexDescriptor::IndexVersion::kV2
                ? kDataFormatV4KeyStringV1UniqueIndexVersionV2
                : kDataFormatV3KeyStringV0UniqueIndexVersionV1;
        } else {
            dataFormatVersion = desc.version() >= IndexDescriptor::IndexVersion::kV2
                ? kDataFormatV6KeyStringV1UniqueIndexVersionV2
                : kDataFormatV5KeyStringV0UniqueIndexVersionV1;
        }
    } else {
        dataFormatVersion = desc.version() >= IndexDescriptor::IndexVersion::kV2
            ? kDataFormatV2KeyStringV1IndexVersionV2
            : kDataFormatV1KeyStringV0IndexVersionV1;
    }

    // Index metadata
    return fmt::format(",app_metadata=(formatVersion={}),", dataFormatVersion);
}

// static
StatusWith<std::string> WiredTigerIndex::generateCreateString(
    const std::string& engineName,
    const std::string& sysIndexConfig,
    const std::string& collIndexConfig,
    const NamespaceString& collectionNamespace,
    const IndexDescriptor& desc,
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
              ->getTableCreateConfig(NamespaceStringUtil::serializeForCatalog(collectionNamespace));
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
    ss << ",key_format=u";
    ss << ",value_format=u";

    // Index metadata
    ss << generateAppMetadataString(desc);
    if (isLogged) {
        ss << "log=(enabled=true)";
    } else {
        ss << "log=(enabled=false)";
    }

    LOGV2_DEBUG(51779, 3, "index create string", "str"_attr = ss.ss.str());
    return StatusWith<std::string>(ss);
}

Status WiredTigerIndex::create(OperationContext* opCtx,
                               const std::string& uri,
                               const std::string& config) {
    // Don't use the session from the recovery unit: create should not be used in a transaction
    WiredTigerSession session(WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn());
    WT_SESSION* s = session.getSession();
    LOGV2_DEBUG(
        51780, 1, "create uri: {uri} config: {config}", "uri"_attr = uri, "config"_attr = config);
    return wtRCToStatus(s->create(s, uri.c_str(), config.c_str()), s);
}

Status WiredTigerIndex::Drop(OperationContext* opCtx, const std::string& uri) {
    WiredTigerSession session(WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn());
    WT_SESSION* s = session.getSession();

    return wtRCToStatus(s->drop(s, uri.c_str(), nullptr), s);
}

WiredTigerIndex::WiredTigerIndex(OperationContext* ctx,
                                 const std::string& uri,
                                 const UUID& collectionUUID,
                                 StringData ident,
                                 KeyFormat rsKeyFormat,
                                 const IndexDescriptor* desc,
                                 bool isLogged)
    : SortedDataInterface(ident,
                          _handleVersionInfo(ctx, uri, ident, desc, isLogged),
                          desc->ordering(),
                          rsKeyFormat),
      _uri(uri),
      _tableId(WiredTigerSession::genTableId()),
      _collectionUUID(collectionUUID),
      _indexName(desc->indexName()),
      _keyPattern(desc->keyPattern()),
      _collation(desc->collation()),
      _isLogged(isLogged) {}

NamespaceString WiredTigerIndex::getCollectionNamespace(OperationContext* opCtx) const {
    // TODO SERVER-73111: Remove CollectionCatalog usage.
    auto nss = CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, _collectionUUID);

    // In testing this may be boost::none.
    if (!nss)
        return NamespaceString::kEmpty;
    return *nss;
}

namespace {
void dassertRecordIdAtEnd(const key_string::Value& keyString, KeyFormat keyFormat) {
    if (!kDebugBuild) {
        return;
    }

    RecordId rid;
    if (keyFormat == KeyFormat::Long) {
        rid = key_string::decodeRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    } else {
        rid = key_string::decodeRecordIdStrAtEnd(keyString.getBuffer(), keyString.getSize());
    }
    invariant(rid.isValid(), rid.toString());
}
}  // namespace

Status WiredTigerIndex::insert(OperationContext* opCtx,
                               const key_string::Value& keyString,
                               bool dupsAllowed,
                               IncludeDuplicateRecordId includeDuplicateRecordId) {
    // Lock invariant relaxed because index builds apply side writes while holding collection MODE_S
    // (global MODE_IS).
    dassert(shard_role_details::getLocker(opCtx)->isLocked());
    dassertRecordIdAtEnd(keyString, _rsKeyFormat);

    LOGV2_TRACE_INDEX(20093, "KeyString: {keyString}", "keyString"_attr = keyString);

    WiredTigerCursor curwrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, false);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();

    return _insert(opCtx, c, keyString, dupsAllowed, includeDuplicateRecordId);
}

void WiredTigerIndex::unindex(OperationContext* opCtx,
                              const key_string::Value& keyString,
                              bool dupsAllowed) {
    // Lock invariant relaxed because index builds apply side writes while holding collection MODE_S
    // (global MODE_IS).
    dassert(shard_role_details::getLocker(opCtx)->isLocked());
    dassertRecordIdAtEnd(keyString, _rsKeyFormat);

    WiredTigerCursor curwrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, false);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    _unindex(opCtx, c, keyString, dupsAllowed);
}

boost::optional<RecordId> WiredTigerIndex::findLoc(OperationContext* opCtx,
                                                   const key_string::Value& key) const {
    auto cursor = newCursor(opCtx);
    return cursor->seekExact(key);
}

IndexValidateResults WiredTigerIndex::validate(OperationContext* opCtx, bool full) const {
    dassert(shard_role_details::getLocker(opCtx)->isReadLocked());

    IndexValidateResults results;
    WiredTigerUtil::validateTableLogging(*WiredTigerRecoveryUnit::get(opCtx),
                                         _uri,
                                         _isLogged,
                                         StringData{_indexName},
                                         results.valid,
                                         results.errors,
                                         results.warnings);

    if (!full) {
        return results;
    }

    WiredTigerIndexUtil::validateStructure(*WiredTigerRecoveryUnit::get(opCtx), _uri, results);

    return results;
}

int64_t WiredTigerIndex::numEntries(OperationContext* opCtx) const {
    int64_t count = 0;

    LOGV2_TRACE_INDEX(20094, "numEntries");

    auto keyInclusion =
        TRACING_ENABLED ? Cursor::KeyInclusion::kInclude : Cursor::KeyInclusion::kExclude;
    auto cursor = newCursor(opCtx);
    for (auto kv = cursor->next(keyInclusion); kv; kv = cursor->next(keyInclusion)) {
        LOGV2_TRACE_INDEX(20095, "numEntries", "kv"_attr = kv);
        count++;
    }

    return count;
}

bool WiredTigerIndex::appendCustomStats(OperationContext* opCtx,
                                        BSONObjBuilder* output,
                                        double scale) const {
    return WiredTigerIndexUtil::appendCustomStats(
        *WiredTigerRecoveryUnit::get(opCtx), output, scale, _uri);
}

Status WiredTigerIndex::dupKeyCheck(OperationContext* opCtx, const key_string::Value& key) {
    invariant(unique());

    WiredTigerCursor curwrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, false);
    WT_CURSOR* c = curwrap.get();

    if (isDup(opCtx, c, key)) {
        return buildDupKeyErrorStatus(
            key, getCollectionNamespace(opCtx), _indexName, _keyPattern, _collation, _ordering);
    }
    return Status::OK();
}

bool WiredTigerIndex::isEmpty(OperationContext* opCtx) {
    return WiredTigerIndexUtil::isEmpty(opCtx, _uri, _tableId);
}

void WiredTigerIndex::printIndexEntryMetadata(OperationContext* opCtx,
                                              const key_string::Value& keyString) const {
    // Printing the index entry metadata requires a new session. We cannot open other cursors when
    // there are open history store cursors in the session. We also need to make sure that the
    // existing session has not written data to avoid potential deadlocks.
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    WiredTigerSession session(WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn());

    // Per the version cursor API:
    // - A version cursor can only be called with the read timestamp as the oldest timestamp.
    // - If there is no oldest timestamp, the version cursor can only be called with a read
    //   timestamp of 1.
    // - If there is an oldest timestamp, reading at timestamp 1 will get rounded up.
    const std::string config = "read_timestamp=1,roundup_timestamps=(read=true)";
    WiredTigerBeginTxnBlock beginTxn(session.getSession(), config.c_str());

    // Open a version cursor. This is a debug cursor that enables iteration through the history of
    // values for a given index entry.
    WT_CURSOR* cursor = session.getNewCursor(_uri, "debug=(dump_version=true)");

    const WiredTigerItem searchKey(keyString.getBuffer(), keyString.getSize());
    cursor->set_key(cursor, searchKey.Get());

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

        auto indexKey = key_string::toBson(
            keyString.getBuffer(), keyString.getSize(), _ordering, keyString.getTypeBits());

        LOGV2(6601200,
              "WiredTiger index entry metadata",
              "keyString"_attr = keyString,
              "indexKey"_attr = indexKey,
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

long long WiredTigerIndex::getSpaceUsedBytes(OperationContext* opCtx) const {
    dassert(shard_role_details::getLocker(opCtx)->isReadLocked());
    auto ru = WiredTigerRecoveryUnit::get(opCtx);
    WT_SESSION* s = ru->getSession()->getSession();

    if (ru->getSessionCache()->isEphemeral()) {
        return static_cast<long long>(WiredTigerUtil::getEphemeralIdentSize(s, _uri));
    }
    return static_cast<long long>(WiredTigerUtil::getIdentSize(s, _uri));
}

long long WiredTigerIndex::getFreeStorageBytes(OperationContext* opCtx) const {
    dassert(shard_role_details::getLocker(opCtx)->isReadLocked());
    auto ru = WiredTigerRecoveryUnit::get(opCtx);
    WiredTigerSession* session = ru->getSessionNoTxn();

    return static_cast<long long>(WiredTigerUtil::getIdentReuseSize(session->getSession(), _uri));
}

Status WiredTigerIndex::initAsEmpty(OperationContext* opCtx) {
    // No-op
    return Status::OK();
}

StatusWith<int64_t> WiredTigerIndex::compact(OperationContext* opCtx,
                                             const CompactOptions& options) {
    return WiredTigerIndexUtil::compact(*opCtx, *WiredTigerRecoveryUnit::get(opCtx), _uri, options);
}

boost::optional<RecordId> WiredTigerIndex::_keyExists(OperationContext* opCtx,
                                                      WT_CURSOR* c,
                                                      const key_string::Value& keyString,
                                                      size_t sizeWithoutRecordId) {
    // Given a KeyString KS with RecordId RID appended to the end, set the:
    // 1. Lower bound (inclusive) to be KS without RID
    // 2. Upper bound (inclusive) to be
    //   a. KS with RecordId::maxLong() for KeyFormat::Long
    //   b. KS with RecordId(FF00) for KeyFormat::String
    //
    // For example, KS = "key" and RID = "ABC123". The lower bound is "key" and the upper bound is
    // "keyFF00".
    WiredTigerItem prefixKeyItem(keyString.getBuffer(), sizeWithoutRecordId);
    setKey(c, prefixKeyItem.Get());
    invariantWTOK(c->bound(c, "bound=lower"), c->session);
    _setUpperBoundForKeyExists(c, keyString, sizeWithoutRecordId);
    ON_BLOCK_EXIT([c] { invariantWTOK(c->bound(c, "action=clear"), c->session); });

    // The cursor is bounded to a prefix. Doing a next on the un-positioned cursor will position on
    // the first key that is equal to or more than the prefix.
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->next(c); });

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneCursorSeek(uri());

    if (ret == WT_NOTFOUND)
        return boost::none;
    invariantWTOK(ret, c->session);

    WT_ITEM key;
    getKey(c, &key, &metricsCollector);
    if (key.size == sizeWithoutRecordId) {
        invariant(_rsKeyFormat == KeyFormat::Long);

        // The prefix key is in the index without a RecordId appended to the key, which means that
        // the RecordId is instead stored in the value.
        WT_ITEM value;
        invariantWTOK(c->get_value(c, &value), c->session);

        BufReader reader(value.data, value.size);
        return key_string::decodeRecordIdLong(&reader);
    }

    return _decodeRecordIdAtEnd(key.data, key.size);
}

void WiredTigerIndex::_setUpperBoundForKeyExists(WT_CURSOR* c,
                                                 const key_string::Value& keyString,
                                                 size_t sizeWithoutRecordId) {
    key_string::Builder builder(keyString.getVersion(), _ordering);
    builder.resetFromBuffer(keyString.getBuffer(), sizeWithoutRecordId);
    builder.appendRecordId(record_id_helpers::maxRecordId(_rsKeyFormat));

    WiredTigerItem upperBoundItem(builder.getBuffer(), builder.getSize());
    setKey(c, upperBoundItem.Get());
    invariantWTOK(c->bound(c, "bound=upper"), c->session);
}

StatusWith<bool> WiredTigerIndex::_checkDups(OperationContext* opCtx,
                                             WT_CURSOR* c,
                                             const key_string::Value& keyString,
                                             IncludeDuplicateRecordId includeDuplicateRecordId) {
    int ret;
    // A prefix key is KeyString of index key. It is the component of the index entry that
    // should be unique.
    auto sizeWithoutRecordId = (_rsKeyFormat == KeyFormat::Long)
        ? key_string::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize())
        : key_string::sizeWithoutRecordIdStrAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem prefixKeyItem(keyString.getBuffer(), sizeWithoutRecordId);

    // First phase inserts the prefix key to prohibit concurrent insertions of same key
    setKey(c, prefixKeyItem.Get());
    c->set_value(c, emptyItem.Get());
    ret = WT_OP_CHECK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(opCtx), c));

    // An entry with prefix key already exists. This can happen only during rolling upgrade when
    // both timestamp unsafe and timestamp safe index format keys could be present.
    if (ret == WT_DUPLICATE_KEY) {
        auto key = key_string::toBson(
            keyString.getBuffer(), sizeWithoutRecordId, _ordering, keyString.getTypeBits());
        return buildDupKeyErrorStatus(
            key, getCollectionNamespace(opCtx), _indexName, _keyPattern, _collation);
    }
    invariantWTOK(ret,
                  c->session,
                  fmt::format("WiredTigerIndex::_insert: insert: {}; uri: {}", _indexName, _uri));

    // Remove the prefix key, our entry will continue to conflict with any concurrent
    // transactions, but will not conflict with any transaction that begins after this
    // operation commits.
    setKey(c, prefixKeyItem.Get());
    ret = WT_OP_CHECK(wiredTigerCursorRemove(*WiredTigerRecoveryUnit::get(opCtx), c));
    invariantWTOK(ret,
                  c->session,
                  fmt::format("WiredTigerIndex::_insert: remove: {}; uri: {}", _indexName, _uri));

    // The second phase looks for the key to avoid insertion of a duplicate key. The range bounded
    // cursor API restricts the key range we search within. This makes the search significantly
    // faster.
    auto rid = _keyExists(opCtx, c, keyString, sizeWithoutRecordId);
    if (!rid) {
        return false;
    } else if (*rid == _decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize())) {
        return true;
    }

    boost::optional<RecordId> foundRecordId = boost::none;
    if (rid && includeDuplicateRecordId == IncludeDuplicateRecordId::kOn) {
        foundRecordId = *rid;
    }

    return buildDupKeyErrorStatus(
        key_string::toBson(
            keyString.getBuffer(), sizeWithoutRecordId, _ordering, keyString.getTypeBits()),
        getCollectionNamespace(opCtx),
        _indexName,
        _keyPattern,
        _collation,
        std::monostate(),
        foundRecordId);
}

void WiredTigerIndex::_repairDataFormatVersion(OperationContext* opCtx,
                                               const std::string& uri,
                                               StringData ident,
                                               const IndexDescriptor* desc) {
    auto indexVersion = desc->version();
    auto isIndexVersion1 = indexVersion == IndexDescriptor::IndexVersion::kV1;
    auto isIndexVersion2 = indexVersion == IndexDescriptor::IndexVersion::kV2;
    auto isDataFormat6 = _dataFormatVersion == kDataFormatV1KeyStringV0IndexVersionV1;
    auto isDataFormat8 = _dataFormatVersion == kDataFormatV2KeyStringV1IndexVersionV2;
    auto isDataFormat13 = _dataFormatVersion == kDataFormatV5KeyStringV0UniqueIndexVersionV1;
    auto isDataFormat14 = _dataFormatVersion == kDataFormatV6KeyStringV1UniqueIndexVersionV2;
    // Only fixes the index data format when it could be from an edge case when converting the
    // uniqueness of the index. Specifically:
    // * The index is a secondary unique index, but the data format version is 6 (v1) or 8 (v2).
    // * The index is a non-unique index, but the data format version is 13 (v1) or 14 (v2).
    if ((!desc->isIdIndex() && desc->unique() &&
         ((isIndexVersion1 && isDataFormat6) || (isIndexVersion2 && isDataFormat8))) ||
        (!desc->unique() &&
         ((isIndexVersion1 && isDataFormat13) || (isIndexVersion2 && isDataFormat14)))) {
        auto engine = opCtx->getServiceContext()->getStorageEngine();
        engine->getEngine()->alterIdentMetadata(
            opCtx, ident, desc, /* isForceUpdateMetadata */ false);
        auto prevVersion = _dataFormatVersion;
        // The updated data format is guaranteed to be within the supported version range.
        _dataFormatVersion = WiredTigerUtil::checkApplicationMetadataFormatVersion(
                                 *WiredTigerRecoveryUnit::get(opCtx),
                                 uri,
                                 kMinimumIndexVersion,
                                 kMaximumIndexVersion)
                                 .getValue();
        LOGV2_WARNING(6818600,
                      "Fixing index metadata data format version",
                      logAttrs(desc->getEntry()->getNSSFromCatalog(opCtx)),
                      "indexName"_attr = desc->indexName(),
                      "prevVersion"_attr = prevVersion,
                      "newVersion"_attr = _dataFormatVersion);
    }
}

key_string::Version WiredTigerIndex::_handleVersionInfo(OperationContext* ctx,
                                                        const std::string& uri,
                                                        StringData ident,
                                                        const IndexDescriptor* desc,
                                                        bool isLogged) {
    auto version = WiredTigerUtil::checkApplicationMetadataFormatVersion(
        *WiredTigerRecoveryUnit::get(ctx), uri, kMinimumIndexVersion, kMaximumIndexVersion);
    if (!version.isOK()) {
        auto collectionNamespace = desc->getEntry()->getNSSFromCatalog(ctx);
        Status versionStatus = version.getStatus();
        Status indexVersionStatus(
            ErrorCodes::UnsupportedFormat,
            str::stream() << versionStatus.reason() << " Index: {name: " << desc->indexName()
                          << ", ns: " << collectionNamespace.toStringForErrorMsg()
                          << "} - version either too old or too new for this mongod.");
        fassertFailedWithStatus(28579, indexVersionStatus);
    }
    _dataFormatVersion = version.getValue();

    _repairDataFormatVersion(ctx, uri, ident, desc);

    if (!desc->isIdIndex() && desc->unique() &&
        (_dataFormatVersion < kDataFormatV3KeyStringV0UniqueIndexVersionV1 ||
         _dataFormatVersion > kDataFormatV6KeyStringV1UniqueIndexVersionV2)) {
        auto collectionNamespace = desc->getEntry()->getNSSFromCatalog(ctx);
        Status versionStatus(ErrorCodes::UnsupportedFormat,
                             str::stream()
                                 << "Index: {name: " << desc->indexName()
                                 << ", ns: " << collectionNamespace.toStringForErrorMsg()
                                 << "} has incompatible format version: " << _dataFormatVersion);
        fassertFailedWithStatusNoTrace(31179, versionStatus);
    }

    uassertStatusOK(
        WiredTigerUtil::setTableLogging(*WiredTigerRecoveryUnit::get(ctx), uri, isLogged));

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

RecordId WiredTigerIndex::_decodeRecordIdAtEnd(const void* buffer, size_t size) {
    switch (_rsKeyFormat) {
        case KeyFormat::Long:
            return key_string::decodeRecordIdLongAtEnd(buffer, size);
        case KeyFormat::String:
            return key_string::decodeRecordIdStrAtEnd(buffer, size);
    }
    MONGO_UNREACHABLE;
}

/**
 * Base class for WiredTigerIndex bulk builders.
 *
 * Manages the bulk cursor used by bulk builders.
 */
class WiredTigerIndex::BulkBuilder : public SortedDataBuilderInterface {
public:
    BulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx)
        : _ordering(idx->_ordering),
          _opCtx(opCtx),
          _metrics(ResourceConsumption::MetricsCollector::get(opCtx)),
          _cursor(*WiredTigerRecoveryUnit::get(_opCtx), idx->uri()) {}

protected:
    void setKey(WT_CURSOR* cursor, const WT_ITEM* item) {
        cursor->set_key(cursor, item);
    }

    const Ordering _ordering;
    OperationContext* const _opCtx;
    ResourceConsumption::MetricsCollector& _metrics;
    WiredTigerBulkLoadCursor _cursor;
};


/**
 * Bulk builds a non-unique index.
 */
class WiredTigerIndex::StandardBulkBuilder : public BulkBuilder {
public:
    StandardBulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx)
        : BulkBuilder(idx, opCtx), _idx(idx) {}

    Status addKey(const key_string::Value& keyString) override {
        dassertRecordIdAtEnd(keyString, _idx->rsKeyFormat());

        // Can't use WiredTigerCursor since we aren't using the cache.
        WiredTigerItem item(keyString.getBuffer(), keyString.getSize());
        setKey(_cursor.get(), item.Get());

        const key_string::TypeBits typeBits = keyString.getTypeBits();
        WiredTigerItem valueItem = typeBits.isAllZeros()
            ? emptyItem
            : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());

        _cursor->set_value(_cursor.get(), valueItem.Get());

        invariantWTOK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(_opCtx), _cursor.get()),
                      _cursor->session);

        _metrics.incrementOneIdxEntryWritten(_idx->uri(), item.size);

        return Status::OK();
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
    UniqueBulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx, bool dupsAllowed)
        : BulkBuilder(idx, opCtx),
          _idx(idx),
          _dupsAllowed(dupsAllowed),
          _previousKeyString(idx->getKeyStringVersion()) {
        invariant(!_idx->isIdIndex());
    }

    Status addKey(const key_string::Value& newKeyString) override {
        dassertRecordIdAtEnd(newKeyString, _idx->rsKeyFormat());

        // Do a duplicate check, but only if dups aren't allowed.
        if (!_dupsAllowed) {
            const int cmp = (_idx->_rsKeyFormat == KeyFormat::Long)
                ? newKeyString.compareWithoutRecordIdLong(_previousKeyString)
                : newKeyString.compareWithoutRecordIdStr(_previousKeyString);
            if (cmp == 0) {
                // Duplicate found!
                auto newKey = key_string::toBson(newKeyString, _idx->_ordering);
                return buildDupKeyErrorStatus(newKey,
                                              _idx->getCollectionNamespace(_opCtx),
                                              _idx->indexName(),
                                              _idx->keyPattern(),
                                              _idx->_collation);
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
        setKey(_cursor.get(), keyItem.Get());

        const key_string::TypeBits typeBits = newKeyString.getTypeBits();
        WiredTigerItem valueItem = typeBits.isAllZeros()
            ? emptyItem
            : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());

        _cursor->set_value(_cursor.get(), valueItem.Get());

        invariantWTOK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(_opCtx), _cursor.get()),
                      _cursor->session);

        _metrics.incrementOneIdxEntryWritten(_idx->uri(), keyItem.size);

        // Don't copy the key again if dups are allowed.
        if (!_dupsAllowed)
            _previousKeyString.resetFromBuffer(newKeyString.getBuffer(), newKeyString.getSize());

        return Status::OK();
    }

private:
    WiredTigerIndex* _idx;
    const bool _dupsAllowed;
    key_string::Builder _previousKeyString;
};

class WiredTigerIndex::IdBulkBuilder : public BulkBuilder {
public:
    IdBulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx)
        : BulkBuilder(idx, opCtx), _idx(idx), _previousKeyString(idx->getKeyStringVersion()) {
        invariant(_idx->isIdIndex());
    }

    Status addKey(const key_string::Value& newKeyString) override {
        dassertRecordIdAtEnd(newKeyString, KeyFormat::Long);

        const int cmp = newKeyString.compareWithoutRecordIdLong(_previousKeyString);
        // _previousKeyString.isEmpty() is only true on the first call to addKey().
        invariant(_previousKeyString.isEmpty() || cmp > 0);

        RecordId id =
            key_string::decodeRecordIdLongAtEnd(newKeyString.getBuffer(), newKeyString.getSize());
        key_string::TypeBits typeBits = newKeyString.getTypeBits();

        key_string::Builder value(_idx->getKeyStringVersion());
        value.appendRecordId(id);
        // When there is only one record, we can omit AllZeros TypeBits. Otherwise they need
        // to be included.
        if (!typeBits.isAllZeros()) {
            value.appendTypeBits(typeBits);
        }

        auto sizeWithoutRecordId = key_string::sizeWithoutRecordIdLongAtEnd(
            newKeyString.getBuffer(), newKeyString.getSize());
        WiredTigerItem keyItem(newKeyString.getBuffer(), sizeWithoutRecordId);
        WiredTigerItem valueItem(value.getBuffer(), value.getSize());

        setKey(_cursor.get(), keyItem.Get());
        _cursor->set_value(_cursor.get(), valueItem.Get());

        invariantWTOK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(_opCtx), _cursor.get()),
                      _cursor->session);

        _metrics.incrementOneIdxEntryWritten(_idx->uri(), keyItem.size);

        _previousKeyString.resetFromBuffer(newKeyString.getBuffer(), newKeyString.getSize());
        return Status::OK();
    }

private:
    WiredTigerIndex* _idx;
    key_string::Builder _previousKeyString;
};

std::unique_ptr<SortedDataBuilderInterface> WiredTigerIdIndex::makeBulkBuilder(
    OperationContext* opCtx, bool dupsAllowed) {
    // Duplicates are not actually allowed on the _id index, however we accept the parameter
    // regardless.
    return std::make_unique<IdBulkBuilder>(this, opCtx);
}

namespace {
/**
 * Implements the basic WT_CURSOR functionality used by both unique and standard indexes.
 */
class WiredTigerIndexCursorBase : public SortedDataInterface::Cursor,
                                  public WiredTigerIndexCursorGeneric {
public:
    WiredTigerIndexCursorBase(const WiredTigerIndex& idx, OperationContext* opCtx, bool forward)
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
          _metrics(&ResourceConsumption::MetricsCollector::get(opCtx)),
          _key(idx.getKeyStringVersion()),
          _typeBits(idx.getKeyStringVersion()) {
        _cursor.emplace(*WiredTigerRecoveryUnit::get(_opCtx), _uri, _tableId, false);
    }

    boost::optional<IndexKeyEntry> next(KeyInclusion keyInclusion) override {
        advanceNext();
        return curr(keyInclusion);
    }

    boost::optional<KeyStringEntry> nextKeyString() override {
        advanceNext();
        return getKeyStringEntry();
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
        const key_string::Value& keyString,
        KeyInclusion keyInclusion = KeyInclusion::kInclude) override {
        seekForKeyStringInternal(keyString);
        return curr(keyInclusion);
    }

    boost::optional<KeyStringEntry> seekForKeyString(
        const key_string::Value& keyStringValue) override {
        seekForKeyStringInternal(keyStringValue);
        return getKeyStringEntry();
    }

    boost::optional<RecordId> seekExact(const key_string::Value& keyString) override {
        dassert(
            key_string::decodeDiscriminator(
                keyString.getBuffer(), keyString.getSize(), _ordering, keyString.getTypeBits()) ==
            key_string::Discriminator::kInclusive);

        seekForKeyStringInternal(keyString);
        if (_eof) {
            return boost::none;
        }

        if (matchesPositionedKey(keyString)) {
            return _id;
        }

        return boost::none;
    }

    void save() override {
        WiredTigerIndexCursorGeneric::resetCursor();

        // Our saved position is wherever we were when we last called updatePosition().
        // Any partially completed repositions should not effect our saved position.
    }

    void saveUnpositioned() override {
        save();
        _eof = true;
    }

    void restore() override {
        if (!_cursor) {
            _cursor.emplace(*WiredTigerRecoveryUnit::get(_opCtx), _uri, _tableId, false);
        }

        // Ensure an active session exists, so any restored cursors will bind to it
        invariant(WiredTigerRecoveryUnit::get(_opCtx)->getSession() == _cursor->getSession());

        if (!_key.isEmpty()) {
            const WiredTigerItem searchKey(_key.getBuffer(), _key.getSize());
            _eof = !seekWTCursorInternal(searchKey);
            if (!_eof) {
                WT_ITEM curKey;
                WT_CURSOR* c = _cursor->get();
                // We don't pass a metrics collector because we don't want to double-count this key
                // read when restoring from yield.
                getKey(c, &curKey, nullptr);

                // cmp is zero when we land on the exact same key we were positioned on before, and
                // non-zero otherwise.
                int cmp = lexCompare(curKey.data, curKey.size, searchKey.data, searchKey.size);
                dassert(_forward ? cmp >= 0 : cmp <= 0);
                _lastMoveSkippedKey = _forward ? cmp > 0 : cmp < 0;
            } else {
                // By setting _lastMoveSkippedKey to true we avoid trying to advance the cursor when
                // we are actually at EOF.
                _lastMoveSkippedKey = true;
            }
            LOGV2_TRACE_CURSOR(20099,
                               "restore _lastMoveSkippedKey: {lastMoveSkippedKey}",
                               "lastMoveSkippedKey"_attr = _lastMoveSkippedKey);
        }
    }

    bool isRecordIdAtEndOfKeyString() const override {
        return true;
    }

    void detachFromOperationContext() override {
        _metrics = nullptr;
        WiredTigerIndexCursorGeneric::detachFromOperationContext();
    }
    void reattachToOperationContext(OperationContext* opCtx) override {
        _metrics = &ResourceConsumption::MetricsCollector::get(opCtx);
        WiredTigerIndexCursorGeneric::reattachToOperationContext(opCtx);
    }
    void setSaveStorageCursorOnDetachFromOperationContext(bool detach) {
        WiredTigerIndexCursorGeneric::setSaveStorageCursorOnDetachFromOperationContext(detach);
    }

    /**
     *  Returns the checkpoint ID for checkpoint cursors, otherwise 0.
     */
    uint64_t getCheckpointId() const {
        return _cursor->getCheckpointId();
    }

protected:
    // Called after _key has been filled in, ie a new key to be processed has been fetched.
    // Must not throw WriteConflictException, throwing a WriteConflictException will retry the
    // operation effectively skipping over this key.
    virtual void updateIdAndTypeBits(const char* newValueData, size_t newValueSize) {
        if (_rsKeyFormat == KeyFormat::Long) {
            _id = key_string::decodeRecordIdLongAtEnd(_key.getBuffer(), _key.getSize());
        } else {
            invariant(_rsKeyFormat == KeyFormat::String);
            _id = key_string::decodeRecordIdStrAtEnd(_key.getBuffer(), _key.getSize());
        }
        invariant(!_id.isNull());

        BufReader br(newValueData, newValueSize);
        _typeBits.resetFromBuffer(&br);
    }

    virtual bool matchesPositionedKey(const key_string::Value& search) const {
        const auto sizeWithoutRecordId = KeyFormat::Long == _rsKeyFormat
            ? key_string::sizeWithoutRecordIdLongAtEnd(_key.getBuffer(), _key.getSize())
            : key_string::sizeWithoutRecordIdStrAtEnd(_key.getBuffer(), _key.getSize());
        return key_string::compare(
                   search.getBuffer(), _key.getBuffer(), search.getSize(), sizeWithoutRecordId) ==
            0;
    }

    boost::optional<IndexKeyEntry> curr(KeyInclusion keyInclusion) const {
        if (_eof)
            return {};

        dassert(!_id.isNull());

        BSONObj bson;
        if (TRACING_ENABLED || keyInclusion == KeyInclusion::kInclude) {
            bson = key_string::toBson(_key.getBuffer(), _key.getSize(), _ordering, _typeBits);

            LOGV2_TRACE_CURSOR(20000, "returning {bson} {id}", "bson"_attr = bson, "id"_attr = _id);
        }

        return {{std::move(bson), _id}};
    }

    // Returns false on EOF and when true, positions the cursor on a key greater than or equal to
    // query, direction dependent.
    [[nodiscard]] bool seekWTCursor(const key_string::Value& query) {
        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(_opCtx)->getSession();

        // We must unposition our cursor by resetting so that we can set new bounds.
        if (_cursor) {
            WiredTigerIndexCursorGeneric::resetCursor();
        }

        const WiredTigerItem searchKey(query.getBuffer(), query.getSize());
        return seekWTCursorInternal(searchKey);
    }

    // Returns false on EOF and when true, positions the cursor on a key greater than or equal to
    // searchKey, direction dependent.
    [[nodiscard]] bool seekWTCursorInternal(const WiredTigerItem& searchKey) {
        WT_CURSOR* cur = _cursor->get();
        if (_endPosition) {
            // Early-return in the unlikely case that our lower bound and upper bound overlap, which
            // is not allowed by the WiredTiger API.
            int cmp = lexCompare(
                searchKey.data, searchKey.size, _endPosition->getBuffer(), _endPosition->getSize());
            if (MONGO_unlikely((_forward && cmp > 0) || (!_forward && cmp < 0))) {
                return false;
            }

            WiredTigerItem endBound(_endPosition->getBuffer(), _endPosition->getSize());
            setKey(cur, endBound.Get());

            // The default bound is inclusive.
            if (_forward) {
                invariantWTOK(cur->bound(cur, "bound=upper"), cur->session);
            } else {
                invariantWTOK(cur->bound(cur, "bound=lower"), cur->session);
            }
        }

        // When seeking with cursors, WiredTiger will traverse over deleted keys until it finds its
        // first non-deleted key. This can make it costly to search for a key that we just
        // deleted if there are many deleted values (e.g. TTL deletes). Additionally, we never want
        // to see a key that comes logically before the last key we returned. Thus, we improve
        // performance by setting a bound to indicate to WiredTiger to only consider returning
        // keys that are relevant to us. The cursor bound is by default inclusive of the key
        // being searched for. This also prevents us from seeing prepared updates on unrelated keys.
        setKey(cur, searchKey.Get());

        // The default bound is inclusive.
        if (_forward) {
            invariantWTOK(cur->bound(cur, "bound=lower"), cur->session);
        } else {
            invariantWTOK(cur->bound(cur, "bound=upper"), cur->session);
        }

        // Our cursor can only move in one direction, so there's no need to clear the bound after
        // seeking. We also don't want to clear our bounds so that the end bound is mainained.
        int ret = wiredTigerPrepareConflictRetry(
            _opCtx, [&] { return _forward ? cur->next(cur) : cur->prev(cur); });

        _metrics->incrementOneCursorSeek(_uri);

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
     */
    void updatePosition(const char* newKeyData,
                        size_t newKeySize,
                        const char* newValueData,
                        size_t newValueSize) {
        // Store (a copy of) the new item data as the current key for this cursor.
        _key.resetFromBuffer(newKeyData, newKeySize);

        if (_eof) {
            return;
        }

        updateIdAndTypeBits(newValueData, newValueSize);
    }

    void checkKeyIsOrdered(const char* newKeyData, size_t newKeySize) {
        if (!kDebugBuild || _key.isEmpty())
            return;
        // In debug mode, let's ensure that our index keys are actually in order. We've had
        // issues in the past with our underlying cursors (WT-2307), but also with cursor
        // mis-use (SERVER-55658). This check can help us catch such things earlier rather than
        // later.
        const int cmp = lexCompare(_key.getBuffer(), _key.getSize(), newKeyData, newKeySize);
        bool outOfOrder = _forward ? cmp > 0 : cmp < 0;

        if (outOfOrder) {
            LOGV2_FATAL(51790,
                        "WTIndex::checkKeyIsOrdered: the new key is out of order with respect to "
                        "the previous key",
                        "newKey"_attr = redact(hexblob::encode(newKeyData, newKeySize)),
                        "prevKey"_attr = redact(_key.toString()),
                        "isForwardCursor"_attr = _forward);
        }
    }


    void seekForKeyStringInternal(const key_string::Value& keyStringValue) {
        dassert(shard_role_details::getLocker(_opCtx)->isReadLocked());
        _eof = !seekWTCursor(keyStringValue);

        _lastMoveSkippedKey = false;
        _id = RecordId();

        if (_eof)
            return;

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        WT_ITEM value;
        getKeyValue(c, &item, &value, _metrics);

        updatePosition(static_cast<const char*>(item.data),
                       item.size,
                       static_cast<const char*>(value.data),
                       value.size);
    }

    void advanceNext() {
        // Advance on a cursor at the end is a no-op.
        if (_eof)
            return;

        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(_opCtx)->getSession();

        if (!_lastMoveSkippedKey)
            _eof = !advanceWTCursor();

        _lastMoveSkippedKey = false;

        if (_eof) {
            // In the normal case, _id will be updated in updatePosition. Making this reset
            // unconditional affects performance noticeably.
            _id = RecordId();
            return;
        }

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        WT_ITEM value;
        getKeyValue(c, &item, &value, _metrics);

        checkKeyIsOrdered(static_cast<const char*>(item.data), item.size);

        updatePosition(static_cast<const char*>(item.data),
                       item.size,
                       static_cast<const char*>(value.data),
                       value.size);
    }

    boost::optional<KeyStringEntry> getKeyStringEntry() {
        if (_eof)
            return {};

        // Most keys will have a RecordId appended to the end, with the exception of the _id index
        // and timestamp unsafe unique indexes. The contract of this function is to always return a
        // KeyString with a RecordId, so append one if it does not exists already.
        if (_unique &&
            // _id index has no RecordId. Unique index version 11 or 12 needs to parse the keys to
            // tell if they are still in old format.
            (_isIdIndex ||
             (_hasOldUniqueIndexFormat &&
              _key.getSize() ==
                  key_string::getKeySize(
                      _key.getBuffer(), _key.getSize(), _ordering, _typeBits)))) {
            // Create a copy of _key with a RecordId. Because _key is used during cursor restore(),
            // appending the RecordId would cause the cursor to be repositioned incorrectly.
            key_string::Builder keyWithRecordId(_key);
            keyWithRecordId.appendRecordId(_id);
            keyWithRecordId.setTypeBits(_typeBits);

            LOGV2_TRACE_CURSOR(20090,
                               "returning {keyWithRecordId} {id}",
                               "keyWithRecordId"_attr = keyWithRecordId,
                               "id"_attr = _id);
            return KeyStringEntry(keyWithRecordId.getValueCopy(), _id);
        }

        _key.setTypeBits(_typeBits);

        LOGV2_TRACE_CURSOR(20091, "returning {key} {id}", "key"_attr = _key, "id"_attr = _id);
        return KeyStringEntry(_key.getValueCopy(), _id);
    }

    NamespaceString getCollectionNamespace(OperationContext* opCtx) const {
        // TODO SERVER-73111: Remove CollectionCatalog usage.
        auto nss = CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, _collectionUUID);
        invariant(nss,
                  str::stream() << "Cannot find namespace for collection with UUID "
                                << _collectionUUID);
        return *nss;
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

    ResourceConsumption::MetricsCollector* _metrics;

    // These are where this cursor instance is. They are not changed in the face of a failing
    // next().
    key_string::Builder _key;
    key_string::TypeBits _typeBits;
    RecordId _id;

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
    WiredTigerIndexUniqueCursor(const WiredTigerIndex& idx, OperationContext* opCtx, bool forward)
        : WiredTigerIndexCursorBase(idx, opCtx, forward) {}

    // Called after _key has been filled in, ie a new key to be processed has been fetched.
    // Must not throw WriteConflictException, throwing a WriteConflictException will retry the
    // operation effectively skipping over this key.
    void updateIdAndTypeBits(const char* newValueData, size_t newValueSize) override {
        LOGV2_TRACE_INDEX(
            20096, "Unique Index KeyString: [{keyString}]", "keyString"_attr = _key.toString());

        // After a rolling upgrade an index can have keys from both timestamp unsafe (old) and
        // timestamp safe (new) unique indexes. Detect correct index key format by checking key's
        // size. Old format keys just had the index key while new format key has index key + Record
        // id. _id indexes remain at the old format. When KeyString contains just the key, the
        // RecordId is in value.
        auto keySize =
            key_string::getKeySize(_key.getBuffer(), _key.getSize(), _ordering, _typeBits);

        if (_key.getSize() == keySize) {
            _updateIdAndTypeBitsFromValue(newValueData, newValueSize);
        } else {
            // The RecordId is in the key at the end. This implementation is provided by the
            // base class, let us just invoke that functionality here.
            WiredTigerIndexCursorBase::updateIdAndTypeBits(newValueData, newValueSize);
        }
    }

    void restore() override {
        // Lets begin by calling the base implementation
        WiredTigerIndexCursorBase::restore();

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
            getKey(c, &item, _metrics);

            // Get the size of the prefix key
            auto keySize =
                key_string::getKeySize(_key.getBuffer(), _key.getSize(), _ordering, _typeBits);

            // This check is only to avoid returning the same key again after a restore. Keys
            // shorter than _key cannot have "prefix key" same as _key. Therefore we care only about
            // the keys with size greater than or equal to that of the _key.
            if (item.size >= keySize && std::memcmp(_key.getBuffer(), item.data, keySize) == 0) {
                _lastMoveSkippedKey = false;
                LOGV2_TRACE_CURSOR(20092, "restore _lastMoveSkippedKey changed to false.");
            }
        }
    }

    bool isRecordIdAtEndOfKeyString() const override {
        return _key.getSize() !=
            key_string::getKeySize(_key.getBuffer(), _key.getSize(), _ordering, _typeBits);
    }

private:
    // Called after _key has been filled in, ie a new key to be processed has been fetched.
    // Must not throw WriteConflictException, throwing a WriteConflictException will retry the
    // operation effectively skipping over this key.
    void _updateIdAndTypeBitsFromValue(const char* newValueData, size_t newValueSize) {
        // Old-format unique index keys always use the Long format.
        invariant(_rsKeyFormat == KeyFormat::Long);

        BufReader br(newValueData, newValueSize);
        _id = key_string::decodeRecordIdLong(&br);
        _typeBits.resetFromBuffer(&br);

        if (!br.atEof()) {
            const auto bsonKey = redact(curr(KeyInclusion::kInclude)->key);
            const auto collectionNamespace = getCollectionNamespace(_opCtx);
            addDataCorruptionEntryToHealthLog(
                _opCtx,
                collectionNamespace,
                "WiredTigerIndexUniqueCursor::_updateIdAndTypeBitsFromValue",
                "Unique index cursor seeing multiple records for key in index",
                bsonKey,
                _indexName,
                _uri);

            LOGV2_ERROR_OPTIONS(7623202,
                                getLogOptionsForDataCorruption(),
                                "Unique index cursor seeing multiple records for key in index",
                                "key"_attr = bsonKey,
                                "index"_attr = _indexName,
                                "uri"_attr = _uri,
                                logAttrs(collectionNamespace));
        }
    }

    virtual bool matchesPositionedKey(const key_string::Value& search) const override {
        // We perform different comparisons depending on whether this is an old-format or new-format
        // key. New-format keys have record IDs at the end.
        if (isRecordIdAtEndOfKeyString()) {
            const auto sizeWithoutRecordId = KeyFormat::Long == _rsKeyFormat
                ? key_string::sizeWithoutRecordIdLongAtEnd(_key.getBuffer(), _key.getSize())
                : key_string::sizeWithoutRecordIdStrAtEnd(_key.getBuffer(), _key.getSize());
            return key_string::compare(search.getBuffer(),
                                       _key.getBuffer(),
                                       search.getSize(),
                                       sizeWithoutRecordId) == 0;
        } else {
            return key_string::compare(
                       search.getBuffer(), _key.getBuffer(), search.getSize(), _key.getSize()) == 0;
        }
    }
};

class WiredTigerIdIndexCursor final : public WiredTigerIndexCursorBase {
public:
    WiredTigerIdIndexCursor(const WiredTigerIndex& idx, OperationContext* opCtx, bool forward)
        : WiredTigerIndexCursorBase(idx, opCtx, forward) {}

    // Called after _key has been filled in, i.e. a new key to be processed has been fetched.
    // Must not throw WriteConflictException, throwing a WriteConflictException will retry the
    // operation effectively skipping over this key.
    void updateIdAndTypeBits(const char* newValueData, size_t newValueSize) override {
        // _id index keys always use the Long format.
        invariant(_rsKeyFormat == KeyFormat::Long);

        BufReader br(newValueData, newValueSize);
        _id = key_string::decodeRecordIdLong(&br);
        _typeBits.resetFromBuffer(&br);

        const auto failWithDataCorruptionForTest =
            WTIndexUassertDuplicateRecordForIdIndex.shouldFail();

        if (!br.atEof() || MONGO_unlikely(failWithDataCorruptionForTest)) {
            const auto bsonKey = redact(curr(KeyInclusion::kInclude)->key);
            const auto collectionNamespace = getCollectionNamespace(_opCtx);

            addDataCorruptionEntryToHealthLog(
                _opCtx,
                collectionNamespace,
                "WiredTigerIdIndexCursor::updateIdAndTypeBits",
                "Index cursor seeing multiple records for key in _id index",
                bsonKey,
                _indexName,
                _uri);

            LOGV2_ERROR_OPTIONS(5176200,
                                getLogOptionsForDataCorruption(failWithDataCorruptionForTest),
                                "Index cursor seeing multiple records for key in _id index",
                                "key"_attr = bsonKey,
                                "index"_attr = _indexName,
                                "uri"_attr = _uri,
                                logAttrs(collectionNamespace));
        }
    }

    virtual bool matchesPositionedKey(const key_string::Value& search) const override {
        return key_string::compare(
                   search.getBuffer(), _key.getBuffer(), search.getSize(), _key.getSize()) == 0;
    }
};
//}  // namespace

WiredTigerIndexUnique::WiredTigerIndexUnique(OperationContext* ctx,
                                             const std::string& uri,
                                             const UUID& collectionUUID,
                                             StringData ident,
                                             KeyFormat rsKeyFormat,
                                             const IndexDescriptor* desc,
                                             bool isLogged)
    : WiredTigerIndex(ctx, uri, collectionUUID, ident, rsKeyFormat, desc, isLogged),
      _partial(desc->isPartial()) {
    // _id indexes must use WiredTigerIdIndex
    invariant(!isIdIndex());
    // All unique indexes should be in the timestamp-safe format version as of version 4.2.
    invariant(isTimestampSafeUniqueIdx());
}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIndexUnique::newCursor(
    OperationContext* opCtx, bool forward) const {
    return std::make_unique<WiredTigerIndexUniqueCursor>(*this, opCtx, forward);
}

std::unique_ptr<SortedDataBuilderInterface> WiredTigerIndexUnique::makeBulkBuilder(
    OperationContext* opCtx, bool dupsAllowed) {
    return std::make_unique<UniqueBulkBuilder>(this, opCtx, dupsAllowed);
}

bool WiredTigerIndexUnique::isTimestampSafeUniqueIdx() const {
    if (_dataFormatVersion == kDataFormatV1KeyStringV0IndexVersionV1 ||
        _dataFormatVersion == kDataFormatV2KeyStringV1IndexVersionV2) {
        return false;
    }
    return true;
}

bool WiredTigerIndexUnique::isDup(OperationContext* opCtx,
                                  WT_CURSOR* c,
                                  const key_string::Value& prefixKey) {
    // This procedure to determine duplicates is exclusive for timestamp safe unique indexes.
    // Check if a prefix key already exists in the index. When keyExists() returns true, the cursor
    // will be positioned on the first occurrence of the 'prefixKey'.
    if (!_keyExists(opCtx, c, prefixKey, prefixKey.getSize())) {
        return false;
    }

    // If the next key also matches, this is a duplicate.
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->next(c); });

    WT_ITEM item;
    if (ret == 0) {
        getKey(c, &item, &ResourceConsumption::MetricsCollector::get(opCtx));
        return std::memcmp(
                   prefixKey.getBuffer(), item.data, std::min(prefixKey.getSize(), item.size)) == 0;
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
                                     const std::string& uri,
                                     const UUID& collectionUUID,
                                     StringData ident,
                                     const IndexDescriptor* desc,
                                     bool isLogged)
    : WiredTigerIndex(ctx, uri, collectionUUID, ident, KeyFormat::Long, desc, isLogged) {
    invariant(isIdIndex());
}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIdIndex::newCursor(OperationContext* opCtx,
                                                                          bool forward) const {
    return std::make_unique<WiredTigerIdIndexCursor>(*this, opCtx, forward);
}

Status WiredTigerIdIndex::_insert(OperationContext* opCtx,
                                  WT_CURSOR* c,
                                  const key_string::Value& keyString,
                                  bool dupsAllowed,
                                  IncludeDuplicateRecordId includeDuplicateRecordId) {
    invariant(KeyFormat::Long == _rsKeyFormat);
    invariant(!dupsAllowed);
    const RecordId id =
        key_string::decodeRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    invariant(id.isValid());

    auto sizeWithoutRecordId =
        key_string::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);

    key_string::Builder value(getKeyStringVersion(), id);
    const key_string::TypeBits typeBits = keyString.getTypeBits();
    if (!typeBits.isAllZeros())
        value.appendTypeBits(typeBits);

    WiredTigerItem valueItem(value.getBuffer(), value.getSize());
    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(opCtx), c));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(_uri, keyItem.size);

    if (ret != WT_DUPLICATE_KEY) {
        return wtRCToStatus(ret, c->session, [this]() {
            return fmt::format("WiredTigerIdIndex::_insert: index: {}; uri: {}", _indexName, _uri);
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

    auto key = key_string::toBson(keyString, _ordering);
    return buildDupKeyErrorStatus(key,
                                  getCollectionNamespace(opCtx),
                                  _indexName,
                                  _keyPattern,
                                  _collation,
                                  std::move(foundValueRecordId),
                                  duplicateRecordId);
}

Status WiredTigerIndexUnique::_insertOldFormatKey(OperationContext* opCtx,
                                                  WT_CURSOR* c,
                                                  const key_string::Value& keyString) {
    const RecordId id =
        key_string::decodeRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    invariant(id.isValid());

    LOGV2_DEBUG(8596201,
                1,
                "Inserting old format key into unique index due to "
                "WTIndexInsertUniqueKeysInOldFormat failpoint",
                "key"_attr = keyString.toString(),
                "recordId"_attr = id.toString(),
                "indexName"_attr = _indexName,
                "keyPattern"_attr = _keyPattern,
                "collectionUUID"_attr = _collectionUUID);

    auto sizeWithoutRecordId =
        key_string::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);

    key_string::Builder value(getKeyStringVersion(), id);
    const key_string::TypeBits typeBits = keyString.getTypeBits();
    if (!typeBits.isAllZeros()) {
        value.appendTypeBits(typeBits);
    }

    WiredTigerItem valueItem(value.getBuffer(), value.getSize());
    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(opCtx), c));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c->uri, keyItem.size);

    // We don't expect WT_DUPLICATE_KEY here as we only insert old format keys when dupsAllowed is
    // false. At this point we will have already triggered a write conflict with any concurrent
    // writers in the new format, so we can safely insert without worrying about duplicates.
    invariantWTOK(ret,
                  c->session,
                  fmt::format("WiredTigerIndexUnique::_insertOldFormatKey error: {}; uri: {}",
                              _indexName,
                              _uri));
    return Status::OK();
}

Status WiredTigerIndexUnique::_insert(OperationContext* opCtx,
                                      WT_CURSOR* c,
                                      const key_string::Value& keyString,
                                      bool dupsAllowed,
                                      IncludeDuplicateRecordId includeDuplicateRecordId) {
    LOGV2_TRACE_INDEX(
        20097, "Timestamp safe unique idx KeyString: {keyString}", "keyString"_attr = keyString);

    int ret;

    // Pre-checks before inserting on a primary.
    if (!dupsAllowed) {
        auto result = _checkDups(opCtx, c, keyString, includeDuplicateRecordId);
        if (!result.isOK()) {
            return result.getStatus();
        } else if (result.getValue()) {
            return Status::OK();
        }
    }

    if (MONGO_unlikely(!dupsAllowed && hasOldFormatVersion() && _rsKeyFormat == KeyFormat::Long &&
                       WTIndexInsertUniqueKeysInOldFormat.shouldFail())) {
        // To support correctness testing of old-format index keys that might still be in the index
        // after an upgrade, this failpoint can be configured to insert keys in the old format,
        // given the required prerequisites. We can't do this when dups are allowed (on
        // secondaries), as this could result in concurrent writes to the same key, the very thing
        // the new format intends to avoid. Note that in testing, the only way to create a unique
        // index with the old format version is with the WTIndexCreateUniqueIndexesInOldFormat
        // failpoint. Old format keys also predated support for KeyFormat::String.
        return _insertOldFormatKey(opCtx, c, keyString);
    }

    // Now create the table key/value, the actual data record.
    WiredTigerItem keyItem(keyString.getBuffer(), keyString.getSize());

    const key_string::TypeBits typeBits = keyString.getTypeBits();
    WiredTigerItem valueItem = typeBits.isAllZeros()
        ? emptyItem
        : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());
    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    ret = WT_OP_CHECK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(opCtx), c));

    // Account for the actual key insertion, but do not attempt account for the complexity of any
    // previous duplicate key detection, which may perform writes.
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(_uri, keyItem.size);

    // It is possible that this key is already present during a concurrent background index build.
    if (ret != WT_DUPLICATE_KEY) {
        invariantWTOK(ret,
                      c->session,
                      fmt::format("WiredTigerIndexUnique::_insert: duplicate: {}; uri: {}",
                                  _indexName,
                                  _uri));
    }

    return Status::OK();
}

void WiredTigerIdIndex::_unindex(OperationContext* opCtx,
                                 WT_CURSOR* c,
                                 const key_string::Value& keyString,
                                 bool dupsAllowed) {
    invariant(KeyFormat::Long == _rsKeyFormat);
    const RecordId id =
        key_string::decodeRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    invariant(id.isValid());

    auto sizeWithoutRecordId =
        key_string::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);
    setKey(c, keyItem.Get());

    const auto failWithDataCorruptionForTest =
        WTIndexUassertDuplicateRecordForKeyOnIdUnindex.shouldFail();
    // On the _id index, the RecordId is stored in the value of the index entry. If the dupsAllowed
    // flag is not set, we blindly delete using only the key without checking the RecordId.
    if (!dupsAllowed && MONGO_likely(!failWithDataCorruptionForTest)) {
        int ret = WT_OP_CHECK(wiredTigerCursorRemove(*WiredTigerRecoveryUnit::get(opCtx), c));
        if (ret == WT_NOTFOUND) {
            return;
        }
        invariantWTOK(ret, c->session);

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
        metricsCollector.incrementOneIdxEntryWritten(_uri, keyItem.size);
        return;
    }

    // Duplicates are never actually allowed on _id indexes, however the 'dupsAllowed' convention
    // requires that we check the value of the RecordId in the keyString instead of blindly deleting
    // based on just the first part of the key.
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret, c->session);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneCursorSeek(_uri);

    WT_ITEM old;
    invariantWTOK(c->get_value(c, &old), c->session);

    BufReader br(old.data, old.size);
    invariant(br.remaining());

    RecordId idInIndex = key_string::decodeRecordIdLong(&br);
    key_string::TypeBits typeBits = key_string::TypeBits::fromBuffer(getKeyStringVersion(), &br);
    if (!br.atEof() || MONGO_unlikely(failWithDataCorruptionForTest)) {
        auto bsonKey = key_string::toBson(keyString, _ordering);
        const auto collectionNamespace = getCollectionNamespace(opCtx);

        addDataCorruptionEntryToHealthLog(opCtx,
                                          collectionNamespace,
                                          "WiredTigerIdIndex::_unindex",
                                          "Un-index seeing multiple records for key",
                                          bsonKey,
                                          _indexName,
                                          _uri);

        LOGV2_ERROR_OPTIONS(5176201,
                            getLogOptionsForDataCorruption(failWithDataCorruptionForTest),
                            "Un-index seeing multiple records for key",
                            "key"_attr = bsonKey,
                            "index"_attr = _indexName,
                            "uri"_attr = _uri,
                            logAttrs(collectionNamespace));
    }

    // The RecordId matches, so remove the entry.
    if (id == idInIndex) {
        invariantWTOK(WT_OP_CHECK(wiredTigerCursorRemove(*WiredTigerRecoveryUnit::get(opCtx), c)),
                      c->session);
        metricsCollector.incrementOneIdxEntryWritten(_uri, keyItem.size);
        return;
    }

    auto key = key_string::toBson(keyString, _ordering);
    LOGV2_WARNING(51797,
                  "Associated record not found in collection while removing index entry",
                  logAttrs(getCollectionNamespace(opCtx)),
                  "index"_attr = _indexName,
                  "key"_attr = redact(key),
                  "recordId"_attr = id);
}

void WiredTigerIndexUnique::_unindex(OperationContext* opCtx,
                                     WT_CURSOR* c,
                                     const key_string::Value& keyString,
                                     bool dupsAllowed) {
    // Note that the dupsAllowed flag asks us to check that the RecordId in the KeyString matches
    // before deleting any keys. Unique indexes store RecordIds in the keyString, so we get this
    // behavior by default.
    WiredTigerItem item(keyString.getBuffer(), keyString.getSize());
    setKey(c, item.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorRemove(*WiredTigerRecoveryUnit::get(opCtx), c));

    // Account for the first removal attempt, but do not attempt to account for the complexity of
    // any subsequent removals and insertions when the index's keys are not fully-upgraded.
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(_uri, item.size);

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
    _unindexTimestampUnsafe(opCtx, c, keyString, dupsAllowed);
}

void WiredTigerIndexUnique::_unindexTimestampUnsafe(OperationContext* opCtx,
                                                    WT_CURSOR* c,
                                                    const key_string::Value& keyString,
                                                    bool dupsAllowed) {
    // The old unique index format had a key-value of indexKey-RecordId. This means that the
    // RecordId in an index entry might not match the indexKey+RecordId keyString passed into this
    // function: an index on a field where multiple collection documents have the same field value
    // but only one passes the partial index filter.
    //
    // The dupsAllowed flag is no longer relevant for the old unique index format. No new index
    // entries are written in the old format, let alone during temporary phases of the server when
    // duplicates are allowed.

    const RecordId id =
        key_string::decodeRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    invariant(id.isValid());

    auto sizeWithoutRecordId =
        key_string::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);
    setKey(c, keyItem.Get());

    if (_partial) {
        int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
        if (ret == WT_NOTFOUND) {
            return;
        }
        invariantWTOK(ret, c->session);

        WT_ITEM value;
        invariantWTOK(c->get_value(c, &value), c->session);
        BufReader br(value.data, value.size);
        fassert(40416, br.remaining());

        // Check that the record id matches. We may be called to unindex records that are not
        // present in the index due to the partial filter expression.
        bool foundRecord = [&]() {
            if (key_string::decodeRecordIdLong(&br) != id) {
                return false;
            }
            return true;
        }();

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
    }

    int ret = WT_OP_CHECK(wiredTigerCursorRemove(*WiredTigerRecoveryUnit::get(opCtx), c));
    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret, c->session);
}
// ------------------------------

WiredTigerIndexStandard::WiredTigerIndexStandard(OperationContext* ctx,
                                                 const std::string& uri,
                                                 const UUID& collectionUUID,
                                                 StringData ident,
                                                 KeyFormat rsKeyFormat,
                                                 const IndexDescriptor* desc,
                                                 bool isLogged)
    : WiredTigerIndex(ctx, uri, collectionUUID, ident, rsKeyFormat, desc, isLogged) {}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIndexStandard::newCursor(
    OperationContext* opCtx, bool forward) const {
    return std::make_unique<WiredTigerIndexStandardCursor>(*this, opCtx, forward);
}

std::unique_ptr<SortedDataBuilderInterface> WiredTigerIndexStandard::makeBulkBuilder(
    OperationContext* opCtx, bool dupsAllowed) {
    // We aren't unique so dups better be allowed.
    invariant(dupsAllowed);
    return std::make_unique<StandardBulkBuilder>(this, opCtx);
}

Status WiredTigerIndexStandard::_insert(OperationContext* opCtx,
                                        WT_CURSOR* c,
                                        const key_string::Value& keyString,
                                        bool dupsAllowed,
                                        IncludeDuplicateRecordId includeDuplicateRecordId) {
    int ret;

    // Pre-checks before inserting on a primary.
    if (!dupsAllowed) {
        auto result = _checkDups(opCtx, c, keyString, includeDuplicateRecordId);
        if (!result.isOK()) {
            return result.getStatus();
        } else if (result.getValue()) {
            return Status::OK();
        }
    }

    WiredTigerItem keyItem(keyString.getBuffer(), keyString.getSize());

    const key_string::TypeBits typeBits = keyString.getTypeBits();
    WiredTigerItem valueItem = typeBits.isAllZeros()
        ? emptyItem
        : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());

    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    ret = WT_OP_CHECK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(opCtx), c));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(_uri, keyItem.size);

    // If the record was already in the index, we return OK. This can happen, for example, when
    // building a background index while documents are being written and reindexed.
    if (!ret || ret == WT_DUPLICATE_KEY) {
        return Status::OK();
    }

    return wtRCToStatus(ret, c->session, [this]() {
        return fmt::format(
            "WiredTigerIndexStandard::_insert: index: {}; uri: {}", _indexName, _uri);
    });
}

void WiredTigerIndexStandard::_unindex(OperationContext* opCtx,
                                       WT_CURSOR* c,
                                       const key_string::Value& keyString,
                                       bool dupsAllowed) {
    invariant(dupsAllowed);
    WiredTigerItem item(keyString.getBuffer(), keyString.getSize());
    setKey(c, item.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorRemove(*WiredTigerRecoveryUnit::get(opCtx), c));

    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret, c->session);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(_uri, item.size);
}

}  // namespace mongo
