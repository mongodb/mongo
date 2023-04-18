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

#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index_cursor_generic.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
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

MONGO_FAIL_POINT_DEFINE(WTIndexPauseAfterSearchNear);

static const WiredTigerItem emptyItem(nullptr, 0);
}  // namespace

void WiredTigerIndex::setKey(WT_CURSOR* cursor, const WT_ITEM* item) {
    cursor->set_key(cursor, item);
}

void WiredTigerIndex::getKey(OperationContext* opCtx, WT_CURSOR* cursor, WT_ITEM* key) {
    invariantWTOK(cursor->get_key(cursor, key), cursor->session);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryRead(_uri, key->size);
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
    StringBuilder ss;

    int keyStringVersion;

    if (desc.unique() && !desc.isIdIndex()) {
        keyStringVersion = desc.version() >= IndexDescriptor::IndexVersion::kV2
            ? kDataFormatV6KeyStringV1UniqueIndexVersionV2
            : kDataFormatV5KeyStringV0UniqueIndexVersionV1;
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

    // Report errors on writes without ordered timestamps.
    ss << "assert=(write_timestamp=on),";
    ss << "verbose=[write_timestamp],";

    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig(collectionNamespace.ns());
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
        return NamespaceString();
    return *nss;
}

namespace {
void dassertRecordIdAtEnd(const KeyString::Value& keyString, KeyFormat keyFormat) {
    if (!kDebugBuild) {
        return;
    }

    RecordId rid;
    if (keyFormat == KeyFormat::Long) {
        rid = KeyString::decodeRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    } else {
        rid = KeyString::decodeRecordIdStrAtEnd(keyString.getBuffer(), keyString.getSize());
    }
    invariant(rid.isValid(), rid.toString());
}
}  // namespace

Status WiredTigerIndex::insert(OperationContext* opCtx,
                               const KeyString::Value& keyString,
                               bool dupsAllowed,
                               IncludeDuplicateRecordId includeDuplicateRecordId) {
    dassert(opCtx->lockState()->isWriteLocked());
    dassertRecordIdAtEnd(keyString, _rsKeyFormat);

    LOGV2_TRACE_INDEX(20093, "KeyString: {keyString}", "keyString"_attr = keyString);

    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();

    return _insert(opCtx, c, keyString, dupsAllowed, includeDuplicateRecordId);
}

void WiredTigerIndex::unindex(OperationContext* opCtx,
                              const KeyString::Value& keyString,
                              bool dupsAllowed) {
    dassert(opCtx->lockState()->isWriteLocked());
    dassertRecordIdAtEnd(keyString, _rsKeyFormat);

    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    _unindex(opCtx, c, keyString, dupsAllowed);
}

boost::optional<RecordId> WiredTigerIndex::findLoc(OperationContext* opCtx,
                                                   const KeyString::Value& key) const {
    dassert(KeyString::decodeDiscriminator(
                key.getBuffer(), key.getSize(), getOrdering(), key.getTypeBits()) ==
            KeyString::Discriminator::kInclusive);

    auto cursor = newCursor(opCtx);
    auto ksEntry = cursor->seekForKeyString(key);
    if (!ksEntry) {
        return boost::none;
    }

    auto sizeWithoutRecordId = KeyFormat::Long == _rsKeyFormat
        ? KeyString::sizeWithoutRecordIdLongAtEnd(ksEntry->keyString.getBuffer(),
                                                  ksEntry->keyString.getSize())
        : KeyString::sizeWithoutRecordIdStrAtEnd(ksEntry->keyString.getBuffer(),
                                                 ksEntry->keyString.getSize());
    if (KeyString::compare(
            ksEntry->keyString.getBuffer(), key.getBuffer(), sizeWithoutRecordId, key.getSize()) ==
        0) {
        return ksEntry->loc;
    }
    return boost::none;
}

IndexValidateResults WiredTigerIndex::validate(OperationContext* opCtx, bool full) const {
    dassert(opCtx->lockState()->isReadLocked());

    IndexValidateResults results;

    WiredTigerUtil::validateTableLogging(opCtx,
                                         _uri,
                                         _isLogged,
                                         StringData{_indexName},
                                         results.valid,
                                         results.errors,
                                         results.warnings);

    if (!full) {
        return results;
    }

    WiredTigerIndexUtil::validateStructure(opCtx, _uri, results);

    return results;
}

int64_t WiredTigerIndex::numEntries(OperationContext* opCtx) const {
    int64_t count = 0;

    LOGV2_TRACE_INDEX(20094, "numEntries");

    auto requestedInfo = TRACING_ENABLED ? Cursor::kKeyAndLoc : Cursor::kJustExistance;
    KeyString::Value keyStringForSeek =
        IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(BSONObj(),
                                                              getKeyStringVersion(),
                                                              getOrdering(),
                                                              true, /* forward */
                                                              true  /* inclusive */
        );

    auto cursor = newCursor(opCtx);
    for (auto kv = cursor->seek(keyStringForSeek, requestedInfo); kv; kv = cursor->next()) {
        LOGV2_TRACE_INDEX(20095, "numEntries", "kv"_attr = kv);
        count++;
    }

    return count;
}

bool WiredTigerIndex::appendCustomStats(OperationContext* opCtx,
                                        BSONObjBuilder* output,
                                        double scale) const {
    return WiredTigerIndexUtil::appendCustomStats(opCtx, output, scale, _uri);
}

Status WiredTigerIndex::dupKeyCheck(OperationContext* opCtx, const KeyString::Value& key) {
    invariant(unique());

    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
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
                                              const KeyString::Value& keyString) const {
    // Printing the index entry metadata requires a new session. We cannot open other cursors when
    // there are open history store cursors in the session. We also need to make sure that the
    // existing session has not written data to avoid potential deadlocks.
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
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

        auto indexKey = KeyString::toBson(
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
    dassert(opCtx->lockState()->isReadLocked());
    auto ru = WiredTigerRecoveryUnit::get(opCtx);
    WT_SESSION* s = ru->getSession()->getSession();

    if (ru->getSessionCache()->isEphemeral()) {
        return static_cast<long long>(WiredTigerUtil::getEphemeralIdentSize(s, _uri));
    }
    return static_cast<long long>(WiredTigerUtil::getIdentSize(s, _uri));
}

long long WiredTigerIndex::getFreeStorageBytes(OperationContext* opCtx) const {
    dassert(opCtx->lockState()->isReadLocked());
    auto ru = WiredTigerRecoveryUnit::get(opCtx);
    WiredTigerSession* session = ru->getSessionNoTxn();

    return static_cast<long long>(WiredTigerUtil::getIdentReuseSize(session->getSession(), _uri));
}

Status WiredTigerIndex::initAsEmpty(OperationContext* opCtx) {
    // No-op
    return Status::OK();
}

Status WiredTigerIndex::compact(OperationContext* opCtx) {
    return WiredTigerIndexUtil::compact(opCtx, _uri);
}

boost::optional<RecordId> WiredTigerIndex::_keyExists(OperationContext* opCtx,
                                                      WT_CURSOR* c,
                                                      const char* buffer,
                                                      size_t size) {
    WiredTigerItem prefixKeyItem(buffer, size);
    setKey(c, prefixKeyItem.Get());

    // An index entry key is KeyString of the prefix key + RecordId. To prevent duplicate prefix
    // key, search a record matching the prefix key.
    int cmp;
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search_near(c, &cmp); });

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneCursorSeek(uri());

    if (ret == WT_NOTFOUND)
        return boost::none;
    invariantWTOK(ret, c->session);

    if (cmp == 0) {
        // The prefix key is in the index without a RecordId appended to the key, which means that
        // the RecordId is instead stored in the value.
        WT_ITEM item;
        invariantWTOK(c->get_value(c, &item), c->session);

        BufReader reader(item.data, item.size);
        return KeyString::decodeRecordIdLong(&reader);
    }

    WT_ITEM item;
    // Obtain the key from the record returned by search near.
    getKey(opCtx, c, &item);
    if (std::memcmp(buffer, item.data, std::min(size, item.size)) == 0) {
        return _decodeRecordIdAtEnd(item.data, item.size);
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
        return boost::none;
    }
    invariantWTOK(ret, c->session);

    getKey(opCtx, c, &item);
    return std::memcmp(buffer, item.data, std::min(size, item.size)) == 0
        ? boost::make_optional(_decodeRecordIdAtEnd(item.data, item.size))
        : boost::none;
}

boost::optional<RecordId> WiredTigerIndex::_keyExistsBounded(OperationContext* opCtx,
                                                             WT_CURSOR* c,
                                                             const KeyString::Value& keyString,
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
    _setUpperBound(c, keyString, sizeWithoutRecordId);
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
    getKey(opCtx, c, &key);
    if (key.size == sizeWithoutRecordId) {
        invariant(_rsKeyFormat == KeyFormat::Long);

        // The prefix key is in the index without a RecordId appended to the key, which means that
        // the RecordId is instead stored in the value.
        WT_ITEM value;
        invariantWTOK(c->get_value(c, &value), c->session);

        BufReader reader(value.data, value.size);
        return KeyString::decodeRecordIdLong(&reader);
    }

    return _decodeRecordIdAtEnd(key.data, key.size);
}

void WiredTigerIndex::_setUpperBound(WT_CURSOR* c,
                                     const KeyString::Value& keyString,
                                     size_t sizeWithoutRecordId) {
    KeyString::Builder builder(keyString.getVersion(), _ordering);
    builder.resetFromBuffer(keyString.getBuffer(), sizeWithoutRecordId);
    builder.appendRecordId(record_id_helpers::maxRecordId(_rsKeyFormat));

    WiredTigerItem upperBoundItem(builder.getBuffer(), builder.getSize());
    setKey(c, upperBoundItem.Get());
    invariantWTOK(c->bound(c, "bound=upper"), c->session);
}

StatusWith<bool> WiredTigerIndex::_checkDups(OperationContext* opCtx,
                                             WT_CURSOR* c,
                                             const KeyString::Value& keyString,
                                             IncludeDuplicateRecordId includeDuplicateRecordId) {
    int ret;
    // A prefix key is KeyString of index key. It is the component of the index entry that
    // should be unique.
    auto sizeWithoutRecordId = (_rsKeyFormat == KeyFormat::Long)
        ? KeyString::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize())
        : KeyString::sizeWithoutRecordIdStrAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem prefixKeyItem(keyString.getBuffer(), sizeWithoutRecordId);

    // First phase inserts the prefix key to prohibit concurrent insertions of same key
    setKey(c, prefixKeyItem.Get());
    c->set_value(c, emptyItem.Get());
    ret = WT_OP_CHECK(wiredTigerCursorInsert(opCtx, c));

    // An entry with prefix key already exists. This can happen only during rolling upgrade when
    // both timestamp unsafe and timestamp safe index format keys could be present.
    if (ret == WT_DUPLICATE_KEY) {
        auto key = KeyString::toBson(
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
    ret = WT_OP_CHECK(wiredTigerCursorRemove(opCtx, c));
    invariantWTOK(ret,
                  c->session,
                  fmt::format("WiredTigerIndex::_insert: remove: {}; uri: {}", _indexName, _uri));

    // The second phase looks for the key to avoid insertion of a duplicate key. The range bounded
    // cursor API restricts the key range we search within. This makes the search significantly
    // faster.
    auto rid = _keyExistsBounded(opCtx, c, keyString, sizeWithoutRecordId);
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
        KeyString::toBson(
            keyString.getBuffer(), sizeWithoutRecordId, _ordering, keyString.getTypeBits()),
        getCollectionNamespace(opCtx),
        _indexName,
        _keyPattern,
        _collation,
        stdx::monostate(),
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
                                 opCtx, uri, kMinimumIndexVersion, kMaximumIndexVersion)
                                 .getValue();
        LOGV2_WARNING(6818600,
                      "Fixing index metadata data format version",
                      logAttrs(desc->getEntry()->getNSSFromCatalog(opCtx)),
                      "indexName"_attr = desc->indexName(),
                      "prevVersion"_attr = prevVersion,
                      "newVersion"_attr = _dataFormatVersion);
    }
}

KeyString::Version WiredTigerIndex::_handleVersionInfo(OperationContext* ctx,
                                                       const std::string& uri,
                                                       StringData ident,
                                                       const IndexDescriptor* desc,
                                                       bool isLogged) {
    auto version = WiredTigerUtil::checkApplicationMetadataFormatVersion(
        ctx, uri, kMinimumIndexVersion, kMaximumIndexVersion);
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

    uassertStatusOK(WiredTigerUtil::setTableLogging(ctx, uri, isLogged));

    /*
     * Index data format 6, 11, and 13 correspond to KeyString version V0 and data format 8, 12, and
     * 14 correspond to KeyString version V1.
     */
    return (_dataFormatVersion == kDataFormatV2KeyStringV1IndexVersionV2 ||
            _dataFormatVersion == kDataFormatV4KeyStringV1UniqueIndexVersionV2 ||
            _dataFormatVersion == kDataFormatV6KeyStringV1UniqueIndexVersionV2)
        ? KeyString::Version::V1
        : KeyString::Version::V0;
}

RecordId WiredTigerIndex::_decodeRecordIdAtEnd(const void* buffer, size_t size) {
    switch (_rsKeyFormat) {
        case KeyFormat::Long:
            return KeyString::decodeRecordIdLongAtEnd(buffer, size);
        case KeyFormat::String:
            return KeyString::decodeRecordIdStrAtEnd(buffer, size);
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
        : _ordering(idx->_ordering), _opCtx(opCtx), _cursor(idx->uri(), _opCtx) {}

protected:
    void setKey(WT_CURSOR* cursor, const WT_ITEM* item) {
        cursor->set_key(cursor, item);
    }

    const Ordering _ordering;
    OperationContext* const _opCtx;
    WiredTigerBulkLoadCursor _cursor;
};


/**
 * Bulk builds a non-unique index.
 */
class WiredTigerIndex::StandardBulkBuilder : public BulkBuilder {
public:
    StandardBulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx)
        : BulkBuilder(idx, opCtx), _idx(idx) {}

    Status addKey(const KeyString::Value& keyString) override {
        dassertRecordIdAtEnd(keyString, _idx->rsKeyFormat());

        // Can't use WiredTigerCursor since we aren't using the cache.
        WiredTigerItem item(keyString.getBuffer(), keyString.getSize());
        setKey(_cursor.get(), item.Get());

        const KeyString::TypeBits typeBits = keyString.getTypeBits();
        WiredTigerItem valueItem = typeBits.isAllZeros()
            ? emptyItem
            : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());

        _cursor->set_value(_cursor.get(), valueItem.Get());

        invariantWTOK(wiredTigerCursorInsert(_opCtx, _cursor.get()), _cursor->session);

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneIdxEntryWritten(_cursor->uri, item.size);

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

    Status addKey(const KeyString::Value& newKeyString) override {
        dassertRecordIdAtEnd(newKeyString, _idx->rsKeyFormat());

        // Do a duplicate check, but only if dups aren't allowed.
        if (!_dupsAllowed) {
            const int cmp = (_idx->_rsKeyFormat == KeyFormat::Long)
                ? newKeyString.compareWithoutRecordIdLong(_previousKeyString)
                : newKeyString.compareWithoutRecordIdStr(_previousKeyString);
            if (cmp == 0) {
                // Duplicate found!
                auto newKey = KeyString::toBson(newKeyString, _idx->_ordering);
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

        const KeyString::TypeBits typeBits = newKeyString.getTypeBits();
        WiredTigerItem valueItem = typeBits.isAllZeros()
            ? emptyItem
            : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());

        _cursor->set_value(_cursor.get(), valueItem.Get());

        invariantWTOK(wiredTigerCursorInsert(_opCtx, _cursor.get()), _cursor->session);

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneIdxEntryWritten(_cursor->uri, keyItem.size);

        // Don't copy the key again if dups are allowed.
        if (!_dupsAllowed)
            _previousKeyString.resetFromBuffer(newKeyString.getBuffer(), newKeyString.getSize());

        return Status::OK();
    }

private:
    WiredTigerIndex* _idx;
    const bool _dupsAllowed;
    KeyString::Builder _previousKeyString;
};

class WiredTigerIndex::IdBulkBuilder : public BulkBuilder {
public:
    IdBulkBuilder(WiredTigerIndex* idx, OperationContext* opCtx)
        : BulkBuilder(idx, opCtx), _idx(idx), _previousKeyString(idx->getKeyStringVersion()) {
        invariant(_idx->isIdIndex());
    }

    Status addKey(const KeyString::Value& newKeyString) override {
        dassertRecordIdAtEnd(newKeyString, KeyFormat::Long);

        const int cmp = newKeyString.compareWithoutRecordIdLong(_previousKeyString);
        // _previousKeyString.isEmpty() is only true on the first call to addKey().
        invariant(_previousKeyString.isEmpty() || cmp > 0);

        RecordId id =
            KeyString::decodeRecordIdLongAtEnd(newKeyString.getBuffer(), newKeyString.getSize());
        KeyString::TypeBits typeBits = newKeyString.getTypeBits();

        KeyString::Builder value(_idx->getKeyStringVersion());
        value.appendRecordId(id);
        // When there is only one record, we can omit AllZeros TypeBits. Otherwise they need
        // to be included.
        if (!typeBits.isAllZeros()) {
            value.appendTypeBits(typeBits);
        }

        auto sizeWithoutRecordId = KeyString::sizeWithoutRecordIdLongAtEnd(newKeyString.getBuffer(),
                                                                           newKeyString.getSize());
        WiredTigerItem keyItem(newKeyString.getBuffer(), sizeWithoutRecordId);
        WiredTigerItem valueItem(value.getBuffer(), value.getSize());

        setKey(_cursor.get(), keyItem.Get());
        _cursor->set_value(_cursor.get(), valueItem.Get());

        invariantWTOK(wiredTigerCursorInsert(_opCtx, _cursor.get()), _cursor->session);

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneIdxEntryWritten(_cursor->uri, keyItem.size);

        _previousKeyString.resetFromBuffer(newKeyString.getBuffer(), newKeyString.getSize());
        return Status::OK();
    }

private:
    WiredTigerIndex* _idx;
    KeyString::Builder _previousKeyString;
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
          _indexName(idx.indexName()),
          _collectionUUID(idx.getCollectionUUID()),
          _key(idx.getKeyStringVersion()),
          _typeBits(idx.getKeyStringVersion()) {
        _cursor.emplace(_uri, _tableId, false, _opCtx);
    }

    boost::optional<IndexKeyEntry> next(RequestedInfo parts) override {
        advanceNext();
        return curr(parts);
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
            ? KeyString::Discriminator::kExclusiveAfter
            : KeyString::Discriminator::kExclusiveBefore;
        _endPosition = std::make_unique<KeyString::Builder>(_version);
        _endPosition->resetToKey(BSONObj::stripFieldNames(key), _ordering, discriminator);
    }

    boost::optional<IndexKeyEntry> seek(const KeyString::Value& keyString,
                                        RequestedInfo parts = kKeyAndLoc) override {
        seekForKeyStringInternal(keyString);
        return curr(parts);
    }

    boost::optional<KeyStringEntry> seekForKeyString(
        const KeyString::Value& keyStringValue) override {
        seekForKeyStringInternal(keyStringValue);
        return getKeyStringEntry();
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
            _cursor.emplace(_uri, _tableId, false, _opCtx);
        }

        // Ensure an active session exists, so any restored cursors will bind to it
        invariant(WiredTigerRecoveryUnit::get(_opCtx)->getSession() == _cursor->getSession());

        if (!_eof) {
            // When using search_near, WiredTiger will traverse over deleted keys until it finds its
            // first non-deleted key. This can make it costly to search for a key that we just
            // deleted if there are many deleted values (e.g. TTL deletes). We never want to see a
            // key that comes logically before the last key we returned. Thus, we improve
            // performance by setting a bound to indicate to WiredTiger to only consider returning
            // keys that are relevant to us. The cursor bound is by default inclusive of the key
            // being searched for so search_near is able to return that key if it exists and avoid
            // looking logically before the last key we returned.
            WT_CURSOR* c = _cursor->get();
            const WiredTigerItem searchKey(_key.getBuffer(), _key.getSize());
            setKey(c, searchKey.Get());
            if (_forward) {
                invariantWTOK(c->bound(c, "bound=lower"), c->session);
            } else {
                invariantWTOK(c->bound(c, "bound=upper"), c->session);
            }

            // Standard (non-unique) indices *do* include the record id in their KeyStrings. This
            // means that restoring to the same key with a new record id will return false, and we
            // will *not* skip the key with the new record id.
            //
            // Unique indexes can have both kinds of KeyStrings, ie with or without the record id.
            // Restore for unique indexes gets handled separately in it's own implementation.
            _lastMoveSkippedKey = !seekWTCursorInternal(searchKey, /*bounded*/ true);
            LOGV2_TRACE_CURSOR(20099,
                               "restore _lastMoveSkippedKey: {lastMoveSkippedKey}",
                               "lastMoveSkippedKey"_attr = _lastMoveSkippedKey);
            invariantWTOK(c->bound(c, "action=clear"), c->session);
        }
    }

    bool isRecordIdAtEndOfKeyString() const override {
        return true;
    }

    void detachFromOperationContext() override {
        WiredTigerIndexCursorGeneric::detachFromOperationContext();
    }
    void reattachToOperationContext(OperationContext* opCtx) override {
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
    virtual void updateIdAndTypeBits() {
        if (_rsKeyFormat == KeyFormat::Long) {
            _id = KeyString::decodeRecordIdLongAtEnd(_key.getBuffer(), _key.getSize());
        } else {
            invariant(_rsKeyFormat == KeyFormat::String);
            _id = KeyString::decodeRecordIdStrAtEnd(_key.getBuffer(), _key.getSize());
        }
        invariant(!_id.isNull());

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        // Can't get WT_ROLLBACK and hence won't throw an exception.
        // Don't expect WT_PREPARE_CONFLICT either.
        auto ret = c->get_value(c, &item);
        invariant(ret != WT_ROLLBACK && ret != WT_PREPARE_CONFLICT);
        invariantWTOK(ret, c->session);
        BufReader br(item.data, item.size);
        _typeBits.resetFromBuffer(&br);
    }

    boost::optional<IndexKeyEntry> curr(RequestedInfo parts) const {
        if (_eof)
            return {};

        dassert(!atOrPastEndPointAfterSeeking());
        dassert(!_id.isNull());

        BSONObj bson;
        if (TRACING_ENABLED || (parts & kWantKey)) {
            bson = KeyString::toBson(_key.getBuffer(), _key.getSize(), _ordering, _typeBits);

            LOGV2_TRACE_CURSOR(20000, "returning {bson} {id}", "bson"_attr = bson, "id"_attr = _id);
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


    // Seeks to query. Returns true on exact match.
    bool seekWTCursor(const KeyString::Value& query) {
        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(_opCtx)->getSession();

        WT_CURSOR* c = _cursor->get();

        const WiredTigerItem searchKey(query.getBuffer(), query.getSize());
        setKey(c, searchKey.Get());
        return seekWTCursorInternal(searchKey, /*bounded*/ false);
    }
    void checkOrderAfterSeek();

    bool seekWTCursorInternal(const WiredTigerItem searchKey, bool bounded) {

        int cmp = -1;
        WT_CURSOR* c = _cursor->get();

        int ret = wiredTigerPrepareConflictRetry(_opCtx, [&] { return c->search_near(c, &cmp); });
        if (ret == WT_NOTFOUND) {
            _cursorAtEof = true;
            LOGV2_TRACE_CURSOR(20088, "not found");
            return false;
        }
        invariantWTOK(ret, c->session);

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneCursorSeek(c->uri);

        _cursorAtEof = false;

        LOGV2_TRACE_CURSOR(20089, "cmp: {cmp}", "cmp"_attr = cmp);

        WTIndexPauseAfterSearchNear.executeIf(
            [](const BSONObj&) {
                LOGV2(5683901, "hanging after search_near");
                WTIndexPauseAfterSearchNear.pauseWhileSet();
            },
            [&](const BSONObj& data) { return data["indexName"].str() == _indexName; });

        if (cmp == 0) {
            // Found it!
            return true;
        }

        dassert(!bounded || (_forward ? cmp >= 0 : cmp <= 0));

        // Make sure we land on a matching key (after/before for forward/reverse).
        // If this operation is ignoring prepared updates and search_near() lands on a key that
        // compares lower than the search key (for a forward cursor), calling next() is not
        // guaranteed to return a key that compares greater than the search key. This is because
        // ignoring prepare conflicts does not provide snapshot isolation and the call to next()
        // may land on a newly-committed prepared entry. We must advance our cursor until we
        // find a key that compares greater than the search key. The same principle applies to
        // reverse cursors. See SERVER-56839.
        const bool enforcingPrepareConflicts =
            _opCtx->recoveryUnit()->getPrepareConflictBehavior() ==
            PrepareConflictBehavior::kEnforce;
        WT_ITEM curKey;
        while (_forward ? cmp < 0 : cmp > 0) {
            _cursorAtEof = advanceWTCursor();
            if (_cursorAtEof) {
                break;
            }

            if (!kDebugBuild && enforcingPrepareConflicts) {
                break;
            }

            getKey(c, &curKey);
            cmp = std::memcmp(curKey.data, searchKey.data, std::min(searchKey.size, curKey.size));

            LOGV2_TRACE_CURSOR(5683900, "cmp after advance: {cmp}", "cmp"_attr = cmp);

            if (enforcingPrepareConflicts) {
                // If we are enforcing prepare conflicts, calling next() or prev() must always
                // give us a key that compares, respectively, greater than or less than our
                // search key. An exact match is also possible in the case of _id indexes,
                // because the recordid is not a part of the key.
                dassert(_forward ? cmp >= 0 : cmp <= 0);
            }
        }

        return false;
    }

    /**
     * This must be called after moving the cursor to update our cached position. It should not
     * be called after a restore that did not restore to original state since that does not
     * logically move the cursor until the following call to next().
     */
    void updatePosition(const char* newKeyData, size_t newKeySize) {
        // Store (a copy of) the new item data as the current key for this cursor.
        _key.resetFromBuffer(newKeyData, newKeySize);

        _eof = atOrPastEndPointAfterSeeking();
        if (_eof)
            return;

        updateIdAndTypeBits();
    }

    void checkKeyIsOrdered(const char* newKeyData, size_t newKeySize) {
        if (!kDebugBuild || _key.isEmpty())
            return;
        // In debug mode, let's ensure that our index keys are actually in order. We've had
        // issues in the past with our underlying cursors (WT-2307), but also with cursor
        // mis-use (SERVER-55658). This check can help us catch such things earlier rather than
        // later.
        const int cmp =
            std::memcmp(_key.getBuffer(), newKeyData, std::min(_key.getSize(), newKeySize));
        bool outOfOrder = _forward ? (cmp > 0 || (cmp == 0 && _key.getSize() > newKeySize))
                                   : (cmp < 0 || (cmp == 0 && _key.getSize() < newKeySize));

        if (outOfOrder) {
            LOGV2_FATAL(51790,
                        "WTIndex::checkKeyIsOrdered: the new key is out of order with respect to "
                        "the previous key",
                        "newKey"_attr = redact(hexblob::encode(newKeyData, newKeySize)),
                        "prevKey"_attr = redact(_key.toString()),
                        "isForwardCursor"_attr = _forward);
        }
    }


    void seekForKeyStringInternal(const KeyString::Value& keyStringValue) {
        dassert(_opCtx->lockState()->isReadLocked());
        seekWTCursor(keyStringValue);

        _lastMoveSkippedKey = false;
        _id = RecordId();

        _eof = _cursorAtEof;
        if (_eof)
            return;

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        getKey(c, &item);

        updatePosition(static_cast<const char*>(item.data), item.size);
    }

    void advanceNext() {
        // Advance on a cursor at the end is a no-op.
        if (_eof)
            return;

        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(_opCtx)->getSession();

        if (!_lastMoveSkippedKey)
            _cursorAtEof = advanceWTCursor();

        _lastMoveSkippedKey = false;

        _eof = _cursorAtEof;
        if (_eof) {
            // In the normal case, _id will be updated in updatePosition. Making this reset
            // unconditional affects performance noticeably.
            _id = RecordId();
            return;
        }

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        getKey(c, &item);

        checkKeyIsOrdered(static_cast<const char*>(item.data), item.size);

        updatePosition(static_cast<const char*>(item.data), item.size);
    }

    boost::optional<KeyStringEntry> getKeyStringEntry() {
        if (_eof)
            return {};

        // Most keys will have a RecordId appended to the end, with the exception of the _id index
        // and timestamp unsafe unique indexes. The contract of this function is to always return a
        // KeyString with a RecordId, so append one if it does not exists already.
        if (_unique &&
            (_isIdIndex ||
             _key.getSize() ==
                 KeyString::getKeySize(_key.getBuffer(), _key.getSize(), _ordering, _typeBits))) {
            // Create a copy of _key with a RecordId. Because _key is used during cursor restore(),
            // appending the RecordId would cause the cursor to be repositioned incorrectly.
            KeyString::Builder keyWithRecordId(_key);
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
    const KeyString::Version _version;
    const KeyFormat _rsKeyFormat;
    const std::string _uri;
    const uint64_t _tableId;
    const bool _unique;
    const bool _isIdIndex;
    const std::string _indexName;
    const UUID _collectionUUID;

    // These are where this cursor instance is. They are not changed in the face of a failing
    // next().
    KeyString::Builder _key;
    KeyString::TypeBits _typeBits;
    RecordId _id;

    std::unique_ptr<KeyString::Builder> _endPosition;

    // This differs from _eof in that it always reflects the result of the most recent call to
    // reposition _cursor.
    bool _cursorAtEof = false;

    // Used by next to decide to return current position rather than moving. Should be reset to
    // false by any operation that moves the cursor, other than subsequent save/restore pairs.
    bool _lastMoveSkippedKey = false;

    bool _eof = true;
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
    void updateIdAndTypeBits() override {
        LOGV2_TRACE_INDEX(
            20096, "Unique Index KeyString: [{keyString}]", "keyString"_attr = _key.toString());

        // After a rolling upgrade an index can have keys from both timestamp unsafe (old) and
        // timestamp safe (new) unique indexes. Detect correct index key format by checking key's
        // size. Old format keys just had the index key while new format key has index key + Record
        // id. _id indexes remain at the old format. When KeyString contains just the key, the
        // RecordId is in value.
        auto keySize =
            KeyString::getKeySize(_key.getBuffer(), _key.getSize(), _ordering, _typeBits);

        if (_key.getSize() == keySize) {
            _updateIdAndTypeBitsFromValue();
        } else {
            // The RecordId is in the key at the end. This implementation is provided by the
            // base class, let us just invoke that functionality here.
            WiredTigerIndexCursorBase::updateIdAndTypeBits();
        }
    }

    void restore() override {
        // Lets begin by calling the base implementation
        WiredTigerIndexCursorBase::restore();

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
            auto keySize =
                KeyString::getKeySize(_key.getBuffer(), _key.getSize(), _ordering, _typeBits);

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
            KeyString::getKeySize(_key.getBuffer(), _key.getSize(), _ordering, _typeBits);
    }

private:
    // Called after _key has been filled in, ie a new key to be processed has been fetched.
    // Must not throw WriteConflictException, throwing a WriteConflictException will retry the
    // operation effectively skipping over this key.
    void _updateIdAndTypeBitsFromValue() {
        // Old-format unique index keys always use the Long format.
        invariant(_rsKeyFormat == KeyFormat::Long);

        // We assume that cursors can only ever see unique indexes in their "pristine" state,
        // where no duplicates are possible. The cases where dups are allowed should hold
        // sufficient locks to ensure that no cursor ever sees them.
        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        // Can't get WT_ROLLBACK and hence won't throw an exception.
        // Don't expect WT_PREPARE_CONFLICT either.
        auto ret = c->get_value(c, &item);
        invariant(ret != WT_ROLLBACK && ret != WT_PREPARE_CONFLICT);
        invariantWTOK(ret, c->session);

        BufReader br(item.data, item.size);
        _id = KeyString::decodeRecordIdLong(&br);
        _typeBits.resetFromBuffer(&br);

        if (!br.atEof()) {
            LOGV2_FATAL(28608,
                        "Unique index cursor seeing multiple records for key {key} in index "
                        "{index} ({uri}) belonging to collection {collection}",
                        "Unique index cursor seeing multiple records for key in index",
                        "key"_attr = redact(curr(kWantKey)->key),
                        "index"_attr = _indexName,
                        "uri"_attr = _uri,
                        logAttrs(getCollectionNamespace(_opCtx)));
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
    void updateIdAndTypeBits() override {
        // _id index keys always use the Long format.
        invariant(_rsKeyFormat == KeyFormat::Long);

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        auto ret = c->get_value(c, &item);
        invariant(ret != WT_ROLLBACK && ret != WT_PREPARE_CONFLICT);
        invariantWTOK(ret, c->session);

        BufReader br(item.data, item.size);
        _id = KeyString::decodeRecordIdLong(&br);
        _typeBits.resetFromBuffer(&br);

        if (!br.atEof()) {
            LOGV2_FATAL(5176200,
                        "Index cursor seeing multiple records for key in _id index",
                        "key"_attr = redact(curr(kWantKey)->key),
                        "index"_attr = _indexName,
                        "uri"_attr = _uri,
                        logAttrs(getCollectionNamespace(_opCtx)));
        }
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
                                  const KeyString::Value& prefixKey) {
    // This procedure to determine duplicates is exclusive for timestamp safe unique indexes.
    // Check if a prefix key already exists in the index. When keyExists() returns true, the cursor
    // will be positioned on the first occurrence of the 'prefixKey'.
    if (!_keyExists(opCtx, c, prefixKey.getBuffer(), prefixKey.getSize())) {
        return false;
    }

    // If the next key also matches, this is a duplicate.
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->next(c); });

    WT_ITEM item;
    if (ret == 0) {
        getKey(opCtx, c, &item);
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

void WiredTigerIndexUnique::insertWithRecordIdInValue_forTest(OperationContext* opCtx,
                                                              const KeyString::Value& keyString,
                                                              RecordId rid) {
    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();

    // Now create the table key/value, the actual data record.
    WiredTigerItem keyItem(keyString.getBuffer(), keyString.getSize());

    KeyString::Builder valueBuilder(keyString.getVersion(), rid);
    valueBuilder.appendTypeBits(keyString.getTypeBits());

    WiredTigerItem valueItem(valueBuilder.getBuffer(), valueBuilder.getSize());
    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorInsert(opCtx, c));

    invariantWTOK(
        ret,
        c->session,
        fmt::format("WiredTigerIndexUnique::insertWithRecordIdInValue_forTest: {}; uri: {}",
                    _indexName,
                    _uri));
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
                                  const KeyString::Value& keyString,
                                  bool dupsAllowed,
                                  IncludeDuplicateRecordId includeDuplicateRecordId) {
    invariant(KeyFormat::Long == _rsKeyFormat);
    invariant(!dupsAllowed);
    const RecordId id =
        KeyString::decodeRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    invariant(id.isValid());

    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);

    KeyString::Builder value(getKeyStringVersion(), id);
    const KeyString::TypeBits typeBits = keyString.getTypeBits();
    if (!typeBits.isAllZeros())
        value.appendTypeBits(typeBits);

    WiredTigerItem valueItem(value.getBuffer(), value.getSize());
    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorInsert(opCtx, c));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c->uri, keyItem.size);

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
        duplicateRecordId = KeyString::decodeRecordIdLong(&reader);
        foundValueRecordId = *duplicateRecordId;
    }

    auto key = KeyString::toBson(keyString, _ordering);
    return buildDupKeyErrorStatus(key,
                                  getCollectionNamespace(opCtx),
                                  _indexName,
                                  _keyPattern,
                                  _collation,
                                  std::move(foundValueRecordId),
                                  duplicateRecordId);
}

Status WiredTigerIndexUnique::_insert(OperationContext* opCtx,
                                      WT_CURSOR* c,
                                      const KeyString::Value& keyString,
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

    // Now create the table key/value, the actual data record.
    WiredTigerItem keyItem(keyString.getBuffer(), keyString.getSize());

    const KeyString::TypeBits typeBits = keyString.getTypeBits();
    WiredTigerItem valueItem = typeBits.isAllZeros()
        ? emptyItem
        : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());
    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    ret = WT_OP_CHECK(wiredTigerCursorInsert(opCtx, c));

    // Account for the actual key insertion, but do not attempt account for the complexity of any
    // previous duplicate key detection, which may perform writes.
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c->uri, keyItem.size);

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
                                 const KeyString::Value& keyString,
                                 bool dupsAllowed) {
    invariant(KeyFormat::Long == _rsKeyFormat);
    const RecordId id =
        KeyString::decodeRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    invariant(id.isValid());

    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);
    setKey(c, keyItem.Get());

    // On the _id index, the RecordId is stored in the value of the index entry. If the dupsAllowed
    // flag is not set, we blindly delete using only the key without checking the RecordId.
    if (!dupsAllowed) {
        int ret = WT_OP_CHECK(wiredTigerCursorRemove(opCtx, c));
        if (ret == WT_NOTFOUND) {
            return;
        }
        invariantWTOK(ret, c->session);

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
        metricsCollector.incrementOneIdxEntryWritten(c->uri, keyItem.size);
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
    metricsCollector.incrementOneCursorSeek(c->uri);

    WT_ITEM old;
    invariantWTOK(c->get_value(c, &old), c->session);

    BufReader br(old.data, old.size);
    invariant(br.remaining());

    RecordId idInIndex = KeyString::decodeRecordIdLong(&br);
    KeyString::TypeBits typeBits = KeyString::TypeBits::fromBuffer(getKeyStringVersion(), &br);
    if (!br.atEof()) {
        auto bsonKey = KeyString::toBson(keyString, _ordering);
        LOGV2_FATAL(5176201,
                    "Un-index seeing multiple records for key",
                    "key"_attr = bsonKey,
                    "index"_attr = _indexName,
                    "uri"_attr = _uri,
                    logAttrs(getCollectionNamespace(opCtx)));
    }

    // The RecordId matches, so remove the entry.
    if (id == idInIndex) {
        invariantWTOK(WT_OP_CHECK(wiredTigerCursorRemove(opCtx, c)), c->session);
        metricsCollector.incrementOneIdxEntryWritten(c->uri, keyItem.size);
        return;
    }

    auto key = KeyString::toBson(keyString, _ordering);
    LOGV2_WARNING(51797,
                  "Associated record not found in collection while removing index entry",
                  logAttrs(getCollectionNamespace(opCtx)),
                  "index"_attr = _indexName,
                  "key"_attr = redact(key),
                  "recordId"_attr = id);
}

void WiredTigerIndexUnique::_unindex(OperationContext* opCtx,
                                     WT_CURSOR* c,
                                     const KeyString::Value& keyString,
                                     bool dupsAllowed) {
    // Note that the dupsAllowed flag asks us to check that the RecordId in the KeyString matches
    // before deleting any keys. Unique indexes store RecordIds in the keyString, so we get this
    // behavior by default.
    WiredTigerItem item(keyString.getBuffer(), keyString.getSize());
    setKey(c, item.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorRemove(opCtx, c));

    // Account for the first removal attempt, but do not attempt to account for the complexity of
    // any subsequent removals and insertions when the index's keys are not fully-upgraded.
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c->uri, item.size);

    if (ret != WT_NOTFOUND) {
        invariantWTOK(ret, c->session);
        return;
    }

    if (KeyFormat::String == _rsKeyFormat) {
        // This is a unique index on a clustered collection. These indexes will only have keys
        // in the timestamp safe format where the RecordId is appended at the end of the key.
        return;
    }

    // After a rolling upgrade an index can have keys from both timestamp unsafe (old) and
    // timestamp safe (new) unique indexes. Old format keys just had the index key while new
    // format key has index key + Record id. WT_NOTFOUND is possible if index key is in old format.
    // Retry removal of key using old format.
    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
    WiredTigerItem keyItem(keyString.getBuffer(), sizeWithoutRecordId);
    setKey(c, keyItem.Get());

    ret = WT_OP_CHECK(wiredTigerCursorRemove(opCtx, c));
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
                                        const KeyString::Value& keyString,
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

    const KeyString::TypeBits typeBits = keyString.getTypeBits();
    WiredTigerItem valueItem = typeBits.isAllZeros()
        ? emptyItem
        : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());

    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    ret = WT_OP_CHECK(wiredTigerCursorInsert(opCtx, c));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c->uri, keyItem.size);

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
                                       const KeyString::Value& keyString,
                                       bool dupsAllowed) {
    invariant(dupsAllowed);
    WiredTigerItem item(keyString.getBuffer(), keyString.getSize());
    setKey(c, item.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorRemove(opCtx, c));

    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret, c->session);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(c->uri, item.size);
}

}  // namespace mongo
