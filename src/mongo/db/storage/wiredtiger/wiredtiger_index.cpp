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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"

#include <memory>
#include <set>

#include "mongo/base/checked_cast.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/json.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#define TRACING_ENABLED 0

#define LOGV2_TRACE_CURSOR(ID, NAME, ...)                   \
    if (TRACING_ENABLED)                                    \
    LOGV2(ID,                                               \
          "WT index ({index}) " #NAME,                      \
          "index"_attr = reinterpret_cast<uint64_t>(&_idx), \
          ##__VA_ARGS__)

#define LOGV2_TRACE_INDEX(ID, NAME, ...)                   \
    if (TRACING_ENABLED)                                   \
    LOGV2(ID,                                              \
          "WT index ({index}) " #NAME,                     \
          "index"_attr = reinterpret_cast<uint64_t>(this), \
          ##__VA_ARGS__)

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(WTCompactIndexEBUSY);
MONGO_FAIL_POINT_DEFINE(WTEmulateOutOfOrderNextIndexKey);
MONGO_FAIL_POINT_DEFINE(WTIndexPauseAfterSearchNear);

using std::string;
using std::vector;

static const WiredTigerItem emptyItem(nullptr, 0);
}  // namespace


// Keystring format 7 was used in 3.3.6 - 3.3.8 development releases. 4.2 onwards, unique indexes
// can be either format version 11 or 12. On upgrading to 4.2, an existing format 6 unique index
// will upgrade to format 11 and an existing format 8 unique index will upgrade to format 12. When
// downgraded from 6.0, unique indexes can also be format 13 or 14. We will allow them to exist but
// we'll still create new unique indexes in format 11 and 12 in version 5.0.
const int kDataFormatV1KeyStringV0IndexVersionV1 = 6;
const int kDataFormatV2KeyStringV1IndexVersionV2 = 8;
const int kDataFormatV3KeyStringV0UniqueIndexVersionV1 = 11;
const int kDataFormatV4KeyStringV1UniqueIndexVersionV2 = 12;
const int kDataFormatV5KeyStringV0UniqueIndexVersionV1 = 13;
const int kDataFormatV6KeyStringV1UniqueIndexVersionV2 = 14;
const int kMinimumIndexVersion = kDataFormatV1KeyStringV0IndexVersionV1;
const int kMaximumIndexVersion = kDataFormatV6KeyStringV1UniqueIndexVersionV2;

void WiredTigerIndex::setKey(WT_CURSOR* cursor, const WT_ITEM* item) {
    cursor->set_key(cursor, item);
}

void WiredTigerIndex::getKey(OperationContext* opCtx, WT_CURSOR* cursor, WT_ITEM* key) {
    invariantWTOK(cursor->get_key(cursor, key));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryRead(key->size);
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
StatusWith<std::string> WiredTigerIndex::generateCreateString(
    const std::string& engineName,
    const std::string& sysIndexConfig,
    const std::string& collIndexConfig,
    const NamespaceString& collectionNamespace,
    const IndexDescriptor& desc) {
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

    bool replicatedWrites = getGlobalReplSettings().usingReplSets() ||
        repl::ReplSettings::shouldRecoverFromOplogAsStandalone();
    if (WiredTigerUtil::useTableLogging(collectionNamespace, replicatedWrites)) {
        ss << "log=(enabled=true)";
    } else {
        ss << "log=(enabled=false)";
    }

    LOGV2_DEBUG(51779, 3, "index create string", "str"_attr = ss.ss.str());
    return StatusWith<std::string>(ss);
}

int WiredTigerIndex::Create(OperationContext* opCtx,
                            const std::string& uri,
                            const std::string& config) {
    // Don't use the session from the recovery unit: create should not be used in a transaction
    WiredTigerSession session(WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn());
    WT_SESSION* s = session.getSession();
    LOGV2_DEBUG(
        51780, 1, "create uri: {uri} config: {config}", "uri"_attr = uri, "config"_attr = config);
    return s->create(s, uri.c_str(), config.c_str());
}

int WiredTigerIndex::Drop(OperationContext* opCtx, const std::string& uri) {
    WiredTigerSession session(WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn());
    WT_SESSION* s = session.getSession();

    return s->drop(s, uri.c_str(), nullptr);
}

WiredTigerIndex::WiredTigerIndex(OperationContext* ctx,
                                 const std::string& uri,
                                 StringData ident,
                                 KeyFormat rsKeyFormat,
                                 const IndexDescriptor* desc,
                                 bool isReadOnly)
    : SortedDataInterface(ident,
                          _handleVersionInfo(ctx, uri, desc, isReadOnly),
                          Ordering::make(desc->keyPattern()),
                          rsKeyFormat),
      _uri(uri),
      _tableId(WiredTigerSession::genTableId()),
      _desc(desc),
      _indexName(desc->indexName()),
      _keyPattern(desc->keyPattern()),
      _collation(desc->collation()) {}

NamespaceString WiredTigerIndex::getCollectionNamespace(OperationContext* opCtx) const {
    return _desc->getEntry()->getNSSFromCatalog(opCtx);
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

StatusWith<bool> WiredTigerIndex::insert(OperationContext* opCtx,
                                         const KeyString::Value& keyString,
                                         bool dupsAllowed) {
    dassert(opCtx->lockState()->isWriteLocked());
    dassertRecordIdAtEnd(keyString, _rsKeyFormat);

    LOGV2_TRACE_INDEX(20093, "KeyString: {keyString}", "keyString"_attr = keyString);

    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();

    return _insert(opCtx, c, keyString, dupsAllowed);
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

void WiredTigerIndex::fullValidate(OperationContext* opCtx,
                                   long long* numKeysOut,
                                   IndexValidateResults* fullResults) const {
    dassert(opCtx->lockState()->isReadLocked());
    if (fullResults && !WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->isEphemeral()) {
        int err = WiredTigerUtil::verifyTable(opCtx, _uri, &(fullResults->errors));
        if (err == EBUSY) {
            std::string msg = str::stream()
                << "Could not complete validation of " << _uri << ". "
                << "This is a transient issue as the collection was actively "
                   "in use by other operations.";

            LOGV2_WARNING(51781,
                          "Could not complete validation. This is a transient issue as "
                          "the collection was actively in use by other operations",
                          "uri"_attr = _uri);
            fullResults->warnings.push_back(msg);
        } else if (err) {
            std::string msg = str::stream()
                << "verify() returned " << wiredtiger_strerror(err) << ". "
                << "This indicates structural damage. "
                << "Not examining individual index entries.";
            LOGV2_ERROR(51782,
                        "verify() returned an error. This indicates structural damage. Not "
                        "examining individual index entries.",
                        "error"_attr = wiredtiger_strerror(err));
            fullResults->errors.push_back(msg);
            fullResults->valid = false;
            return;
        }
    }

    auto cursor = newCursor(opCtx);
    long long count = 0;
    LOGV2_TRACE_INDEX(20094, "fullValidate");

    const auto requestedInfo = TRACING_ENABLED ? Cursor::kKeyAndLoc : Cursor::kJustExistance;

    KeyString::Value keyStringForSeek =
        IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(BSONObj(),
                                                              getKeyStringVersion(),
                                                              getOrdering(),
                                                              true, /* forward */
                                                              true  /* inclusive */
        );

    for (auto kv = cursor->seek(keyStringForSeek, requestedInfo); kv; kv = cursor->next()) {
        LOGV2_TRACE_INDEX(20095, "fullValidate {kv}", "kv"_attr = kv);
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
    StatusWith<std::string> metadataResult = WiredTigerUtil::getMetadataCreate(opCtx, sourceURI);
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

Status WiredTigerIndex::dupKeyCheck(OperationContext* opCtx, const KeyString::Value& key) {
    invariant(unique());

    WiredTigerCursor curwrap(_uri, _tableId, false, opCtx);
    WT_CURSOR* c = curwrap.get();

    if (isDup(opCtx, c, key)) {
        auto entry = _desc->getEntry();
        auto nss = entry ? entry->getNSSFromCatalog(opCtx) : NamespaceString();
        return buildDupKeyErrorStatus(key, nss, _indexName, _keyPattern, _collation, _ordering);
    }
    return Status::OK();
}

bool WiredTigerIndex::isEmpty(OperationContext* opCtx) {
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

long long WiredTigerIndex::getFreeStorageBytes(OperationContext* opCtx) const {
    dassert(opCtx->lockState()->isReadLocked());
    auto ru = WiredTigerRecoveryUnit::get(opCtx);
    WiredTigerSession* session = ru->getSession();

    return static_cast<long long>(WiredTigerUtil::getIdentReuseSize(session->getSession(), _uri));
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
        if (MONGO_unlikely(WTCompactIndexEBUSY.shouldFail())) {
            ret = EBUSY;
        }

        if (ret == EBUSY) {
            return Status(ErrorCodes::Interrupted,
                          str::stream() << "Compaction interrupted on " << uri().c_str()
                                        << " due to cache eviction pressure");
        }
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
        auto collectionNamespace = desc->getEntry()->getNSSFromCatalog(ctx);
        Status versionStatus = version.getStatus();
        Status indexVersionStatus(ErrorCodes::UnsupportedFormat,
                                  str::stream()
                                      << versionStatus.reason() << " Index: {name: "
                                      << desc->indexName() << ", ns: " << collectionNamespace
                                      << "} - version either too old or too new for this mongod.");
        fassertFailedWithStatus(28579, indexVersionStatus);
    }
    _dataFormatVersion = version.getValue();

    if (!desc->isIdIndex() && desc->unique() &&
        (_dataFormatVersion < kDataFormatV3KeyStringV0UniqueIndexVersionV1 ||
         _dataFormatVersion > kDataFormatV6KeyStringV1UniqueIndexVersionV2)) {
        auto collectionNamespace = desc->getEntry()->getNSSFromCatalog(ctx);
        Status versionStatus(ErrorCodes::UnsupportedFormat,
                             str::stream()
                                 << "Index: {name: " << desc->indexName()
                                 << ", ns: " << collectionNamespace
                                 << "} has incompatible format version: " << _dataFormatVersion);
        fassertFailedWithStatusNoTrace(31179, versionStatus);
    }

    if (!isReadOnly) {
        bool replicatedWrites = getGlobalReplSettings().usingReplSets() ||
            repl::ReplSettings::shouldRecoverFromOplogAsStandalone();
        bool useTableLogging = !replicatedWrites ||
            WiredTigerUtil::useTableLogging(desc->getEntry()->getNSSFromCatalog(ctx),
                                            replicatedWrites);
        uassertStatusOK(WiredTigerUtil::setTableLogging(ctx, uri, useTableLogging));
    }

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
          _session(WiredTigerRecoveryUnit::get(_opCtx)->getSessionCache()->getSession()),
          _cursor(openBulkCursor(idx)) {}

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

        LOGV2_WARNING(51783,
                      "failed to create WiredTiger bulk cursor: {error} falling back to non-bulk "
                      "cursor for index {index}",
                      "Failed to create WiredTiger bulk cursor, falling back to non-bulk",
                      "error"_attr = wiredtiger_strerror(err),
                      "index"_attr = idx->uri());

        invariantWTOK(session->open_cursor(session, idx->uri().c_str(), nullptr, nullptr, &cursor));
        return cursor;
    }

    void setKey(WT_CURSOR* cursor, const WT_ITEM* item) {
        cursor->set_key(cursor, item);
    }

    const Ordering _ordering;
    OperationContext* const _opCtx;
    UniqueWiredTigerSession const _session;
    WT_CURSOR* const _cursor;
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
        setKey(_cursor, item.Get());

        const KeyString::TypeBits typeBits = keyString.getTypeBits();
        WiredTigerItem valueItem = typeBits.isAllZeros()
            ? emptyItem
            : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());

        _cursor->set_value(_cursor, valueItem.Get());

        invariantWTOK(wiredTigerCursorInsert(_opCtx, _cursor));

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneIdxEntryWritten(item.size);

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
        dassertRecordIdAtEnd(newKeyString, KeyFormat::Long);

        // Do a duplicate check, but only if dups aren't allowed.
        if (!_dupsAllowed) {
            const int cmp = newKeyString.compareWithoutRecordIdLong(_previousKeyString);
            if (cmp == 0) {
                // Duplicate found!
                auto newKey = KeyString::toBson(newKeyString, _idx->_ordering);
                auto entry = _idx->_desc->getEntry();
                return buildDupKeyErrorStatus(newKey,
                                              entry ? entry->getNSSFromCatalog(_opCtx)
                                                    : NamespaceString(),
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
        setKey(_cursor, keyItem.Get());

        const KeyString::TypeBits typeBits = newKeyString.getTypeBits();
        WiredTigerItem valueItem = typeBits.isAllZeros()
            ? emptyItem
            : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());

        _cursor->set_value(_cursor, valueItem.Get());

        invariantWTOK(wiredTigerCursorInsert(_opCtx, _cursor));

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneIdxEntryWritten(keyItem.size);

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

        setKey(_cursor, keyItem.Get());
        _cursor->set_value(_cursor, valueItem.Get());

        invariantWTOK(wiredTigerCursorInsert(_opCtx, _cursor));

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneIdxEntryWritten(keyItem.size);

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
class WiredTigerIndexCursorBase : public SortedDataInterface::Cursor {
public:
    WiredTigerIndexCursorBase(const WiredTigerIndex& idx, OperationContext* opCtx, bool forward)
        : _opCtx(opCtx),
          _idx(idx),
          _forward(forward),
          _key(idx.getKeyStringVersion()),
          _typeBits(idx.getKeyStringVersion()),
          _query(idx.getKeyStringVersion()) {
        _cursor.emplace(_idx.uri(), _idx.tableId(), false, _opCtx);
    }

    boost::optional<IndexKeyEntry> next(RequestedInfo parts) override {
        if (!advanceNext()) {
            return {};
        }
        return curr(parts);
    }

    boost::optional<KeyStringEntry> nextKeyString() override {
        if (!advanceNext()) {
            return {};
        }
        if (_eof) {
            return {};
        }

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
        _endPosition = std::make_unique<KeyString::Builder>(_idx.getKeyStringVersion());
        _endPosition->resetToKey(BSONObj::stripFieldNames(key), _idx.getOrdering(), discriminator);
    }

    boost::optional<IndexKeyEntry> seek(const KeyString::Value& keyString,
                                        RequestedInfo parts = kKeyAndLoc) override {
        seekForKeyString(keyString);
        return curr(parts);
    }

    boost::optional<KeyStringEntry> seekForKeyString(
        const KeyString::Value& keyStringValue) override {
        dassert(_opCtx->lockState()->isReadLocked());
        seekWTCursor(keyStringValue);

        updatePosition();
        if (_eof)
            return {};

        dassert(!atOrPastEndPointAfterSeeking());
        dassert(!_id.isNull());

        return getKeyStringEntry();
    }

    boost::optional<KeyStringEntry> seekExactForKeyString(const KeyString::Value& key) override {
        dassert(KeyString::decodeDiscriminator(
                    key.getBuffer(), key.getSize(), _idx.getOrdering(), key.getTypeBits()) ==
                KeyString::Discriminator::kInclusive);

        auto ksEntry = [&]() {
            if (_forward) {
                return seekForKeyString(key);
            }

            // Append a kExclusiveAfter discriminator if it's a reverse cursor to ensure that the
            // KeyString we construct will always be greater than the KeyString that we retrieve
            // (even when it has a RecordId).
            KeyString::Builder keyCopy(_idx.getKeyStringVersion(), _idx.getOrdering());

            // Reset by copying all but the last byte, the kEnd byte.
            keyCopy.resetFromBuffer(key.getBuffer(), key.getSize() - 1);

            // Append a different discriminator and new end byte.
            keyCopy.appendDiscriminator(KeyString::Discriminator::kExclusiveAfter);
            return seekForKeyString(keyCopy.getValueCopy());
        }();

        if (!ksEntry) {
            return {};
        }

        if (KeyString::compare(ksEntry->keyString.getBuffer(),
                               key.getBuffer(),
                               KeyString::sizeWithoutRecordIdLongAtEnd(
                                   ksEntry->keyString.getBuffer(), ksEntry->keyString.getSize()),
                               key.getSize()) == 0) {
            return KeyStringEntry(ksEntry->keyString, ksEntry->loc);
        }
        return {};
    }

    boost::optional<IndexKeyEntry> seekExact(const KeyString::Value& keyStringValue,
                                             RequestedInfo parts) override {
        auto ksEntry = seekExactForKeyString(keyStringValue);
        if (ksEntry) {
            auto kv = curr(parts);
            invariant(kv);
            return kv;
        }
        return {};
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
            _lastMoveSkippedKey = !seekWTCursor(_key.getValueCopy());
            LOGV2_TRACE_CURSOR(20099,
                               "restore _lastMoveSkippedKey: {lastMoveSkippedKey}",
                               "lastMoveSkippedKey"_attr = _lastMoveSkippedKey);
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
        if (_idx.rsKeyFormat() == KeyFormat::Long) {
            _id = KeyString::decodeRecordIdLongAtEnd(_key.getBuffer(), _key.getSize());
        } else {
            invariant(_idx.rsKeyFormat() == KeyFormat::String);
            _id = KeyString::decodeRecordIdStrAtEnd(_key.getBuffer(), _key.getSize());
        }

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
        cursor->set_key(cursor, item);
    }

    void getKey(WT_CURSOR* cursor, WT_ITEM* key) {
        invariantWTOK(cursor->get_key(cursor, key));

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneIdxEntryRead(key->size);
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

    void advanceWTCursor() {
        WT_CURSOR* c = _cursor->get();
        int ret = wiredTigerPrepareConflictRetry(
            _opCtx, [&] { return _forward ? c->next(c) : c->prev(c); });
        if (ret == WT_NOTFOUND) {
            _cursorAtEof = true;
            return;
        }
        invariantWTOK(ret);
        _cursorAtEof = false;
    }

    // Seeks to query. Returns true on exact match.
    bool seekWTCursor(const KeyString::Value& query) {
        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(_opCtx)->getSession();

        WT_CURSOR* c = _cursor->get();

        int cmp = -1;
        const WiredTigerItem searchKey(query.getBuffer(), query.getSize());
        setKey(c, searchKey.Get());

        int ret = wiredTigerPrepareConflictRetry(_opCtx, [&] { return c->search_near(c, &cmp); });
        if (ret == WT_NOTFOUND) {
            _cursorAtEof = true;
            LOGV2_TRACE_CURSOR(20088, "not found");
            return false;
        }
        invariantWTOK(ret);

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
        metricsCollector.incrementOneCursorSeek();

        _cursorAtEof = false;

        LOGV2_TRACE_CURSOR(20089, "cmp: {cmp}", "cmp"_attr = cmp);

        WTIndexPauseAfterSearchNear.executeIf(
            [](const BSONObj&) {
                LOGV2(5683901, "hanging after search_near");
                WTIndexPauseAfterSearchNear.pauseWhileSet();
            },
            [&](const BSONObj& data) { return data["indexName"].str() == _idx.indexName(); });

        if (cmp == 0) {
            // Found it!
            return true;
        }

        // Make sure we land on a matching key (after/before for forward/reverse).
        // If this operation is ignoring prepared updates and search_near() lands on a key that
        // compares lower than the search key (for a forward cursor), calling next() is not
        // guaranteed to return a key that compares greater than the search key. This is because
        // ignoring prepare conflicts does not provide snapshot isolation and the call to next()
        // may land on a newly-committed prepared entry. We must advance our cursor until we find a
        // key that compares greater than the search key. The same principle applies to reverse
        // cursors. See SERVER-56839.
        const bool enforcingPrepareConflicts =
            _opCtx->recoveryUnit()->getPrepareConflictBehavior() ==
            PrepareConflictBehavior::kEnforce;
        WT_ITEM curKey;
        while (_forward ? cmp < 0 : cmp > 0) {
            advanceWTCursor();
            if (_cursorAtEof) {
                break;
            }

            if (!kDebugBuild && enforcingPrepareConflicts) {
                break;
            }

            getKey(c, &curKey);
            cmp = std::memcmp(curKey.data, searchKey.data, std::min(searchKey.size, curKey.size));

            LOGV2_TRACE_CURSOR(5683900, "cmp after advance: {cmp}", "cmp"_attr = cmp);

            // We do not expect any exact matches or matches of prefixes by comparing keys of
            // different lengths. Callers either seek using keys with discriminators that always
            // compare unequally, or in the case of restoring a cursor, perform exact searches. In
            // the case of an exact search, we will have returned earlier.
            dassert(cmp);

            if (enforcingPrepareConflicts) {
                // If we are enforcing prepare conflicts, calling next() or prev() must always give
                // us a key that compares, respectively, greater than or less than our search key.
                dassert(_forward ? cmp > 0 : cmp < 0);
            }
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

            if (MONGO_unlikely(WTEmulateOutOfOrderNextIndexKey.shouldFail())) {
                LOGV2(51789, "WTIndex::updatePosition simulating next key not increasing.");
                nextNotIncreasing = true;
            }

            if (nextNotIncreasing) {
                // Our new key is less than the old key which means the next call moved to !next.
                LOGV2_ERROR(
                    51790,
                    "WTIndex::updatePosition -- the new key ({newKey}) is less than the previous "
                    "key ({prevKey}), which is a bug.",
                    "WTIndex::updatePosition -- new key is less than previous key",
                    "newKey"_attr = redact(hexblob::encode(item.data, item.size)),
                    "prevKey"_attr = redact(_key.toString()));

                // Crash when testing diagnostics are enabled.
                invariant(!TestingProctor::instance().isEnabled());

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

    bool advanceNext() {
        // Advance on a cursor at the end is a no-op.
        if (_eof) {
            return false;
        }

        // Ensure an active transaction is open.
        WiredTigerRecoveryUnit::get(_opCtx)->getSession();

        if (!_lastMoveSkippedKey) {
            advanceWTCursor();
        }
        updatePosition(true);
        return true;
    }

    KeyStringEntry getKeyStringEntry() {
        // Most keys will have a RecordId appended to the end, with the exception of the _id index
        // and timestamp unsafe unique indexes. The contract of this function is to always return a
        // KeyString with a RecordId, so append one if it does not exists already.
        if (_idx.unique() &&
            (_idx.isIdIndex() ||
             _key.getSize() ==
                 KeyString::getKeySize(
                     _key.getBuffer(), _key.getSize(), _idx.getOrdering(), _typeBits))) {
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

    std::unique_ptr<KeyString::Builder> _endPosition;
};

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
            KeyString::getKeySize(_key.getBuffer(), _key.getSize(), _idx.getOrdering(), _typeBits);

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
            auto keySize = KeyString::getKeySize(
                _key.getBuffer(), _key.getSize(), _idx.getOrdering(), _typeBits);

            // This check is only to avoid returning the same key again after a restore. Keys
            // shorter than _key cannot have "prefix key" same as _key. Therefore we care only about
            // the keys with size greater than or equal to that of the _key.
            if (item.size >= keySize && std::memcmp(_key.getBuffer(), item.data, keySize) == 0) {
                _lastMoveSkippedKey = false;
                LOGV2_TRACE_CURSOR(20092, "restore _lastMoveSkippedKey changed to false.");
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
        _id = KeyString::decodeRecordIdLong(&br);
        _typeBits.resetFromBuffer(&br);

        if (!br.atEof()) {
            LOGV2_FATAL(28608,
                        "Unique index cursor seeing multiple records for key {key} in index "
                        "{index} ({uri}) belonging to collection {collection}",
                        "Unique index cursor seeing multiple records for key in index",
                        "key"_attr = redact(curr(kWantKey)->key),
                        "index"_attr = _idx.indexName(),
                        "uri"_attr = _idx.uri(),
                        "collection"_attr = _idx.getCollectionNamespace(_opCtx));
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
        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        auto ret = c->get_value(c, &item);
        invariant(ret != WT_ROLLBACK && ret != WT_PREPARE_CONFLICT);
        invariantWTOK(ret);

        BufReader br(item.data, item.size);
        _id = KeyString::decodeRecordIdLong(&br);
        _typeBits.resetFromBuffer(&br);

        if (!br.atEof()) {
            LOGV2_FATAL(5176200,
                        "Index cursor seeing multiple records for key in _id index",
                        "key"_attr = redact(curr(kWantKey)->key),
                        "index"_attr = _idx.indexName(),
                        "uri"_attr = _idx.uri(),
                        "collection"_attr = _idx.getCollectionNamespace(_opCtx));
        }
    }
};
}  // namespace

WiredTigerIndexUnique::WiredTigerIndexUnique(OperationContext* ctx,
                                             const std::string& uri,
                                             StringData ident,
                                             const IndexDescriptor* desc,
                                             bool isReadOnly)
    : WiredTigerIndex(ctx, uri, ident, KeyFormat::Long, desc, isReadOnly),
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

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneCursorSeek();

    if (cmp == 0)
        return true;

    WT_ITEM item;
    // Obtain the key from the record returned by search near.
    getKey(opCtx, c, &item);
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

    getKey(opCtx, c, &item);
    return std::memcmp(buffer, item.data, std::min(size, item.size)) == 0;
}

bool WiredTigerIndexUnique::isDup(OperationContext* opCtx,
                                  WT_CURSOR* c,
                                  const KeyString::Value& prefixKey) {
    // This procedure to determine duplicates is exclusive for timestamp safe unique indexes.
    // Check if a prefix key already exists in the index. When keyExists() returns true, the cursor
    // will be positioned on the first occurence of the 'prefixKey'.
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

    fassertFailedWithStatus(40685, wtRCToStatus(ret));
    MONGO_UNREACHABLE;
}

WiredTigerIdIndex::WiredTigerIdIndex(OperationContext* ctx,
                                     const std::string& uri,
                                     StringData ident,
                                     const IndexDescriptor* desc,
                                     bool isReadOnly)
    : WiredTigerIndex(ctx, uri, ident, KeyFormat::Long, desc, isReadOnly) {
    invariant(isIdIndex());
}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIdIndex::newCursor(OperationContext* opCtx,
                                                                          bool forward) const {
    return std::make_unique<WiredTigerIdIndexCursor>(*this, opCtx, forward);
}

StatusWith<bool> WiredTigerIdIndex::_insert(OperationContext* opCtx,
                                            WT_CURSOR* c,
                                            const KeyString::Value& keyString,
                                            bool dupsAllowed) {
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
    metricsCollector.incrementOneIdxEntryWritten(keyItem.size);

    if (!ret) {
        return true;
    } else if (ret != WT_DUPLICATE_KEY) {
        return wtRCToStatus(ret);
    }

    auto key = KeyString::toBson(keyString, _ordering);
    return buildDupKeyErrorStatus(
        key, _desc->getEntry()->getNSSFromCatalog(opCtx), _indexName, _keyPattern, _collation);
}

StatusWith<bool> WiredTigerIndexUnique::_insert(OperationContext* opCtx,
                                                WT_CURSOR* c,
                                                const KeyString::Value& keyString,
                                                bool dupsAllowed) {
    LOGV2_TRACE_INDEX(
        20097, "Timestamp safe unique idx KeyString: {keyString}", "keyString"_attr = keyString);

    int ret;

    // Pre-checks before inserting on a primary.
    if (!dupsAllowed) {
        // A prefix key is KeyString of index key. It is the component of the index entry that
        // should be unique.
        auto sizeWithoutRecordId =
            KeyString::sizeWithoutRecordIdLongAtEnd(keyString.getBuffer(), keyString.getSize());
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
            return buildDupKeyErrorStatus(key,
                                          _desc->getEntry()->getNSSFromCatalog(opCtx),
                                          _indexName,
                                          _keyPattern,
                                          _collation);
        }
        invariantWTOK(ret);

        // Remove the prefix key, our entry will continue to conflict with any concurrent
        // transactions, but will not conflict with any transaction that begins after this
        // operation commits.
        setKey(c, prefixKeyItem.Get());
        ret = WT_OP_CHECK(wiredTigerCursorRemove(opCtx, c));
        invariantWTOK(ret);

        // Second phase looks up for existence of key to avoid insertion of duplicate key
        if (_keyExists(opCtx, c, keyString.getBuffer(), sizeWithoutRecordId)) {
            auto key = KeyString::toBson(
                keyString.getBuffer(), sizeWithoutRecordId, _ordering, keyString.getTypeBits());
            auto entry = _desc->getEntry();
            return buildDupKeyErrorStatus(key,
                                          entry ? entry->getNSSFromCatalog(opCtx)
                                                : NamespaceString(),
                                          _indexName,
                                          _keyPattern,
                                          _collation);
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
    metricsCollector.incrementOneIdxEntryWritten(keyItem.size);

    // It is possible that this key is already present during a concurrent background index build.
    if (ret == WT_DUPLICATE_KEY) {
        return false;
    }

    invariantWTOK(ret);

    return true;
}

void WiredTigerIdIndex::_unindex(OperationContext* opCtx,
                                 WT_CURSOR* c,
                                 const KeyString::Value& keyString,
                                 bool dupsAllowed) {
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
        invariantWTOK(ret);

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
        metricsCollector.incrementOneIdxEntryWritten(keyItem.size);
        return;
    }

    // Duplicates are never actually allowed on _id indexes, however the 'dupsAllowed' convention
    // requires that we check the value of the RecordId in the keyString instead of blindly deleting
    // based on just the first part of the key.
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
    if (ret == WT_NOTFOUND) {
        return;
    }
    invariantWTOK(ret);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneCursorSeek();

    WT_ITEM old;
    invariantWTOK(c->get_value(c, &old));

    BufReader br(old.data, old.size);
    invariant(br.remaining());

    RecordId idInIndex = KeyString::decodeRecordIdLong(&br);
    KeyString::TypeBits typeBits = KeyString::TypeBits::fromBuffer(getKeyStringVersion(), &br);
    if (!br.atEof()) {
        auto bsonKey = KeyString::toBson(keyString, _ordering);
        LOGV2_FATAL(5176201,
                    "Un-index seeing multiple records for key",
                    "key"_attr = bsonKey,
                    "index"_attr = _desc->indexName(),
                    "uri"_attr = _uri,
                    "collection"_attr = getCollectionNamespace(opCtx));
    }

    // The RecordId matches, so remove the entry.
    if (id == idInIndex) {
        invariantWTOK(WT_OP_CHECK(wiredTigerCursorRemove(opCtx, c)));
        metricsCollector.incrementOneIdxEntryWritten(keyItem.size);
        return;
    }

    auto key = KeyString::toBson(keyString, _ordering);
    LOGV2_WARNING(51797,
                  "Associated record not found in collection while removing index entry",
                  "collection"_attr = getCollectionNamespace(opCtx),
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
    metricsCollector.incrementOneIdxEntryWritten(item.size);

    if (ret != WT_NOTFOUND) {
        invariantWTOK(ret);
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
    invariantWTOK(ret);
}
// ------------------------------

WiredTigerIndexStandard::WiredTigerIndexStandard(OperationContext* ctx,
                                                 const std::string& uri,
                                                 StringData ident,
                                                 KeyFormat rsKeyFormat,
                                                 const IndexDescriptor* desc,
                                                 bool isReadOnly)
    : WiredTigerIndex(ctx, uri, ident, rsKeyFormat, desc, isReadOnly) {}

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

StatusWith<bool> WiredTigerIndexStandard::_insert(OperationContext* opCtx,
                                                  WT_CURSOR* c,
                                                  const KeyString::Value& keyString,
                                                  bool dupsAllowed) {
    invariant(dupsAllowed);

    WiredTigerItem keyItem(keyString.getBuffer(), keyString.getSize());

    const KeyString::TypeBits typeBits = keyString.getTypeBits();
    WiredTigerItem valueItem = typeBits.isAllZeros()
        ? emptyItem
        : WiredTigerItem(typeBits.getBuffer(), typeBits.getSize());

    setKey(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    int ret = WT_OP_CHECK(wiredTigerCursorInsert(opCtx, c));

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(keyItem.size);

    // If the record was already in the index, return false.
    // This can happen, for example, when building a background index while documents are being
    // written and reindexed.
    if (ret == WT_DUPLICATE_KEY) {
        return false;
    } else if (ret) {
        return wtRCToStatus(ret);
    }

    return true;
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
    invariantWTOK(ret);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneIdxEntryWritten(item.size);
}

}  // namespace mongo
