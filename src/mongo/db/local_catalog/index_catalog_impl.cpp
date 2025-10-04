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

#include "mongo/db/local_catalog/index_catalog_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/aggregated_index_usage_tracker.h"
#include "mongo/db/audit.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index/s2_bucket_access_method.h"
#include "mongo/db/index_builds/index_build_block.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry_impl.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/repl_set_member_in_standalone_mode.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/ttl/ttl_collection_cache.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update_index_data.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/numeric/conversion/converter_policies.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo {

MONGO_FAIL_POINT_DEFINE(skipUnindexingDocumentWhenDeleted);
MONGO_FAIL_POINT_DEFINE(skipIndexNewRecords);
MONGO_FAIL_POINT_DEFINE(skipUpdatingIndexDocument);

// This failpoint causes the check for TTL indexes on capped collections to be ignored.
MONGO_FAIL_POINT_DEFINE(ignoreTTLIndexCappedCollectionCheck);

using IndexVersion = IndexDescriptor::IndexVersion;

const BSONObj IndexCatalogImpl::_idObj = BSON("_id" << 1);

namespace {
/**
 * Similar to _isSpecOK(), checks if the indexSpec is valid, conflicts, or already exists as a
 * clustered index.
 *
 * Returns Status::OK() if no clustered index exists or the 'indexSpec' does not conflict with it.
 * Returns ErrorCodes::IndexAlreadyExists if the 'indexSpec' already exists as the clustered index.
 * Returns an error if the indexSpec fields conflict with the clustered index.
 */
Status isSpecOKClusteredIndexCheck(const BSONObj& indexSpec,
                                   const boost::optional<ClusteredCollectionInfo>& collInfo) {
    auto key = indexSpec.getObjectField("key");
    bool keysMatch = clustered_util::matchesClusterKey(key, collInfo);

    bool clusteredOptionPresent = indexSpec.hasField(IndexDescriptor::kClusteredFieldName) &&
        indexSpec[IndexDescriptor::kClusteredFieldName].trueValue();

    if (clusteredOptionPresent && !keysMatch) {
        // The 'clustered' option implies the indexSpec must match the clustered index.
        return Status(ErrorCodes::Error(6243700),
                      "Cannot create index with option 'clustered' that does not match an existing "
                      "clustered index");
    }

    auto name = indexSpec.getStringField("name");
    bool namesMatch = !collInfo.has_value() || collInfo->getIndexSpec().getName().value() == name;


    if (!keysMatch && !namesMatch) {
        // The indexes don't conflict at all.
        return Status::OK();
    }

    if (!collInfo) {
        return Status(ErrorCodes::Error(6479600),
                      str::stream() << "Cannot create an index with 'clustered' in the spec on a "
                                    << "collection that is not clustered");
    }

    // The collection is guaranteed to be clustered since at least the name or key matches a
    // clustered index.
    auto clusteredIndexSpec = collInfo->getIndexSpec();

    if (namesMatch && !keysMatch) {
        // Prohibit creating an index with the same 'name' as the cluster key but different key
        // pattern.
        return Status(ErrorCodes::Error(6100906),
                      str::stream() << "Cannot create an index where the name matches the "
                                       "clusteredIndex but the key does not -"
                                    << " indexSpec: " << indexSpec
                                    << ", clusteredIndex: " << collInfo->getIndexSpec().toBSON());
    }

    // Users should be able to call createIndexes on the cluster key. If a name isn't specified, a
    // default one is generated. Silently ignore mismatched names.

    BSONElement vElt = indexSpec["v"];
    auto version = representAs<int>(vElt.number());
    if (clusteredIndexSpec.getV() != version) {
        return Status(ErrorCodes::Error(6100908),
                      "Cannot create an index with the same key pattern as the collection's "
                      "clusteredIndex but a different 'v' field");
    }

    if (indexSpec.hasField("unique") && indexSpec.getBoolField("unique") == false) {
        return Status(ErrorCodes::Error(6100909),
                      "Cannot create an index with the same key pattern as the collection's "
                      "clusteredIndex but a different 'unique' field");
    }

    // The indexSpec matches the clustered index, which already exists implicitly.
    return Status(ErrorCodes::IndexAlreadyExists,
                  "The index already exists implicitly as the collection's clustered index");
};

using IndexIterator = IndexCatalog::IndexIterator;

class ReadyIndexesIterator : public IndexIterator {
public:
    ReadyIndexesIterator(IndexCatalogEntryContainer::const_iterator beginIterator,
                         IndexCatalogEntryContainer::const_iterator endIterator)
        : _iterator(beginIterator), _endIterator(endIterator) {}

private:
    IndexCatalogEntryContainer::const_iterator _iterator;
    IndexCatalogEntryContainer::const_iterator _endIterator;

    const IndexCatalogEntry* _advance() override {
        return _iterator == _endIterator ? nullptr : _iterator++->get();
    }
};

class AllIndexesIterator : public IndexIterator {
public:
    AllIndexesIterator(std::vector<const IndexCatalogEntry*> container)
        : _container(std::move(container)), _iterator(_container.begin()) {}

private:
    std::vector<const IndexCatalogEntry*> _container;
    std::vector<const IndexCatalogEntry*>::const_iterator _iterator;

    const IndexCatalogEntry* _advance() override {
        return _iterator == _container.end() ? nullptr : *_iterator++;
    }
};
}  // namespace

// -------------

std::unique_ptr<IndexCatalog> IndexCatalogImpl::clone() const {
    return std::make_unique<IndexCatalogImpl>(*this);
}

void IndexCatalogImpl::init(OperationContext* opCtx,
                            Collection* collection,
                            bool isPointInTimeRead) {
    invariant(!haveAnyIndexes());

    std::vector<std::string> indexNames;
    collection->getAllIndexes(&indexNames);
    const bool replSetMemberInStandaloneMode =
        getReplSetMemberInStandaloneMode(opCtx->getServiceContext());

    boost::optional<Timestamp> recoveryTs = boost::none;
    if (auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        storageEngine->supportsRecoveryTimestamp()) {
        recoveryTs = storageEngine->getRecoveryTimestamp();
    }

    for (auto& indexName : indexNames) {
        BSONObj spec = collection->getIndexSpec(indexName).getOwned();
        BSONObj keyPattern = spec.getObjectField("key");

        if (IndexNames::findPluginName(keyPattern) == IndexNames::COLUMN) {
            LOGV2_OPTIONS(7281100,
                          {logv2::LogTag::kStartupWarnings},
                          "Found a columnstore index in the catalog. Columnstore indexes are no "
                          "longer supported and are not recommended for production use.",
                          "ns"_attr = collection->ns(),
                          "uuid"_attr = collection->uuid(),
                          "index"_attr = indexName,
                          "spec"_attr = spec);
        }

        auto descriptor = IndexDescriptor(_getAccessMethodName(keyPattern), spec);

        if (spec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName)) {
            // TTL indexes with an invalid 'expireAfterSeconds' field cause problems in multiversion
            // settings.
            auto swType = index_key_validate::validateExpireAfterSeconds(
                spec[IndexDescriptor::kExpireAfterSecondsFieldName],
                index_key_validate::ValidateExpireAfterSecondsMode::kSecondaryTTLIndex);
            auto expireAfterSecondsType = index_key_validate::extractExpireAfterSecondsType(swType);
            if (expireAfterSecondsType ==
                TTLCollectionCache::Info::ExpireAfterSecondsType::kInvalid) {
                LOGV2_OPTIONS(
                    6852200,
                    {logv2::LogTag::kStartupWarnings},
                    "Found an existing TTL index with invalid 'expireAfterSeconds' in the "
                    "catalog.",
                    "ns"_attr = collection->ns(),
                    "uuid"_attr = collection->uuid(),
                    "index"_attr = indexName,
                    "spec"_attr = spec);
            }
            // Note that TTL deletion is supported on capped clustered collections via bounded
            // collection scan, which does not use an index.
            if (shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork()) {
                shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                    [svcCtx = opCtx->getServiceContext(),
                     uuid = collection->uuid(),
                     indexName,
                     expireAfterSecondsType](OperationContext*, boost::optional<Timestamp>) {
                        TTLCollectionCache::get(svcCtx).registerTTLInfo(
                            uuid, TTLCollectionCache::Info{indexName, expireAfterSecondsType});
                    });
            } else {
                TTLCollectionCache::get(opCtx->getServiceContext())
                    .registerTTLInfo(collection->uuid(),
                                     TTLCollectionCache::Info{indexName, expireAfterSecondsType});
            }
        }

        bool ready = collection->isIndexReady(indexName);
        try {
            if (!ready) {
                if (!isPointInTimeRead) {
                    // When initializing the indexes at the latest timestamp for existing
                    // collections, the only non-ready indexes will be two-phase index builds.
                    // Unfinished single-phase index builds are dropped during startup and rollback.
                    auto buildUUID = collection->getIndexBuildUUID(indexName);
                    invariant(buildUUID,
                              str::stream()
                                  << "collection: " << collection->ns().toStringForErrorMsg()
                                  << "index:" << indexName);
                }

                // We intentionally do not drop or rebuild unfinished two-phase index builds before
                // initializing the IndexCatalog when starting a replica set member in standalone
                // mode. This is because the index build cannot complete until it receives a
                // replicated commit or abort oplog entry. When performing a point-in-time read,
                // this non-ready index may represent a single-phase index build.
                if (replSetMemberInStandaloneMode) {
                    // Indicate that this index is "frozen". It is not ready but is not currently in
                    // progress either. These indexes may be dropped.
                    auto flags =
                        CreateIndexEntryFlags::kInitFromDisk | CreateIndexEntryFlags::kFrozen;
                    IndexCatalogEntry* entry =
                        createIndexEntry(opCtx, collection, std::move(descriptor), flags);
                    fassert(31433, !entry->isReady());
                } else {
                    // Initializing with unfinished indexes may occur during rollback or startup.
                    auto flags = CreateIndexEntryFlags::kInitFromDisk;
                    IndexCatalogEntry* entry =
                        createIndexEntry(opCtx, collection, std::move(descriptor), flags);
                    fassert(4505500, !entry->isReady());
                }
            } else {
                auto flags = CreateIndexEntryFlags::kInitFromDisk | CreateIndexEntryFlags::kIsReady;
                IndexCatalogEntry* entry =
                    createIndexEntry(opCtx, collection, std::move(descriptor), flags);
                fassert(17340, entry->isReady());
            }
        } catch (const ExceptionFor<ErrorCodes::CannotCreateIndex>& ex) {
            // Offline validation runs against older versions of MongoDB, so index registration
            // failures are tolerable. The known case is for GeoHaystack indexes in <4.4
            if (!storageGlobalParams.validate) {
                throw;
            }
            LOGV2_ERROR(9534800,
                        "Failed to register index. It will not be validated",
                        "ns"_attr = collection->ns(),
                        "indexName"_attr = indexName,
                        "error"_attr = ex.toString());
            continue;
        }
    }

    // When instantiating a collection for point-in-time reads the collection instance can be using
    // shared state, so we clear the query plan cache and rebuild it.
    CollectionQueryInfo& info = CollectionQueryInfo::get(collection);
    if (isPointInTimeRead) {
        info.clearQueryCache(opCtx, CollectionPtr(collection));
    }
    info.init(opCtx, collection);
}

std::unique_ptr<IndexCatalog::IndexIterator> IndexCatalogImpl::getIndexIterator(
    InclusionPolicy inclusionPolicy) const {
    if (inclusionPolicy == InclusionPolicy::kReady) {
        // Queries need to be able to efficiently iterate over ready indexes, so special-case that
        // with a more efficient version that doesn't need to copy anything
        return std::make_unique<ReadyIndexesIterator>(_readyIndexes.begin(), _readyIndexes.end());
    }

    // If the caller doesn't only want the ready indexes, for simplicity of implementation, we copy
    // the pointers to a new vector. The vector's ownership is passed to the iterator. The query
    // code path from an external client is not expected to hit this case so the cost isn't paid by
    // the important code path.
    std::vector<const IndexCatalogEntry*> allIndexes;

    if (inclusionPolicy & InclusionPolicy::kReady) {
        for (const auto& entry : _readyIndexes) {
            allIndexes.push_back(entry.get());
        }
    }

    if (inclusionPolicy & InclusionPolicy::kUnfinished) {
        for (const auto& entry : _buildingIndexes) {
            allIndexes.push_back(entry.get());
        }
    }

    if (inclusionPolicy & InclusionPolicy::kFrozen) {
        for (const auto& entry : _frozenIndexes) {
            allIndexes.push_back(entry.get());
        }
    }

    return std::make_unique<AllIndexesIterator>(std::move(allIndexes));
}

std::string IndexCatalogImpl::_getAccessMethodName(const BSONObj& keyPattern) const {
    std::string pluginName = IndexNames::findPluginName(keyPattern);

    // This assert will be triggered when downgrading from a future version that
    // supports an index plugin unsupported by this version.
    uassert(17197,
            str::stream() << "Invalid index type '" << pluginName << "' "
                          << "in index " << keyPattern,
            IndexNames::isKnownName(pluginName));

    return pluginName;
}

// ---------------------------

StatusWith<BSONObj> IndexCatalogImpl::_validateAndFixIndexSpec(OperationContext* opCtx,
                                                               const CollectionPtr& collection,
                                                               const BSONObj& original) const {
    Status status = _isSpecOk(opCtx, collection, original);
    if (!status.isOK()) {
        return status;
    }

    auto swFixed = _fixIndexSpec(opCtx, collection, original);
    if (!swFixed.isOK()) {
        return swFixed;
    }

    // we double check with new index spec
    status = _isSpecOk(opCtx, collection, swFixed.getValue());
    if (!status.isOK()) {
        return status;
    }

    return swFixed;
}

Status IndexCatalogImpl::_isNonIDIndexAndNotAllowedToBuild(OperationContext* opCtx,
                                                           const BSONObj& spec) const {
    const BSONObj key = spec.getObjectField("key");
    invariant(!key.isEmpty());
    if (IndexDescriptor::isIdIndexPattern(key)) {
        return Status::OK();
    }

    if (!getGlobalReplSettings().isReplSet()) {
        return Status::OK();
    }

    // Check whether the replica set member's config has {buildIndexes:false} set, which means
    // we are not allowed to build non-_id indexes on this server.
    if (!repl::ReplicationCoordinator::get(opCtx)->buildsIndexes()) {
        // We return an IndexAlreadyExists error so that the caller can catch it and silently
        // skip building it.
        return Status(ErrorCodes::IndexAlreadyExists,
                      "this replica set member's 'buildIndexes' setting is set to false");
    }

    return Status::OK();
}

void IndexCatalogImpl::_logInternalState(OperationContext* opCtx,
                                         const CollectionPtr& collection,
                                         long long numIndexesInCollectionCatalogEntry,
                                         const std::vector<std::string>& indexNamesToDrop) {
    invariant(
        shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(collection->ns(), MODE_X));

    LOGV2_ERROR(20365,
                "Internal Index Catalog state",
                "numIndexesTotal"_attr = numIndexesTotal(),
                "numIndexesInCollectionCatalogEntry"_attr = numIndexesInCollectionCatalogEntry,
                "numReadyIndexes"_attr = _readyIndexes.size(),
                "numBuildingIndexes"_attr = _buildingIndexes.size(),
                "numFrozenIndexes"_attr = _frozenIndexes.size(),
                "indexNamesToDrop"_attr = indexNamesToDrop);

    // Report the ready indexes.
    for (const auto& entry : _readyIndexes) {
        const IndexDescriptor* desc = entry->descriptor();
        LOGV2_ERROR(20367,
                    "readyIndex",
                    "index"_attr = desc->indexName(),
                    "indexInfo"_attr = redact(desc->infoObj()));
    }

    // Report the in-progress indexes.
    for (const auto& entry : _buildingIndexes) {
        const IndexDescriptor* desc = entry->descriptor();
        LOGV2_ERROR(20369,
                    "buildingIndex",
                    "index"_attr = desc->indexName(),
                    "indexInfo"_attr = redact(desc->infoObj()));
    }

    LOGV2_ERROR(20370, "Internal Collection Catalog Entry state:");
    std::vector<std::string> allIndexes;
    std::vector<std::string> readyIndexes;

    collection->getAllIndexes(&allIndexes);
    collection->getReadyIndexes(&readyIndexes);

    for (const auto& index : allIndexes) {
        LOGV2_ERROR(20372,
                    "allIndexes",
                    "index"_attr = index,
                    "spec"_attr = redact(collection->getIndexSpec(index)));
    }

    for (const auto& index : readyIndexes) {
        LOGV2_ERROR(20374,
                    "readyIndexes",
                    "index"_attr = index,
                    "spec"_attr = redact(collection->getIndexSpec(index)));
    }
}

StatusWith<BSONObj> IndexCatalogImpl::prepareSpecForCreate(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const BSONObj& original,
    const boost::optional<ResumeIndexInfo>& resumeInfo) const {
    auto swValidatedAndFixed = _validateAndFixIndexSpec(opCtx, collection, original);
    if (!swValidatedAndFixed.isOK()) {
        return swValidatedAndFixed.getStatus().withContext(
            str::stream() << "Error in specification " << original.toString());
    }

    auto validatedSpec = swValidatedAndFixed.getValue();

    // Check whether this is a non-_id index and there are any settings disallowing this server
    // from building non-_id indexes.
    Status status = _isNonIDIndexAndNotAllowedToBuild(opCtx, validatedSpec);
    if (!status.isOK()) {
        return status;
    }

    // First check against only the ready indexes for conflicts.
    status =
        _doesSpecConflictWithExisting(opCtx, collection, validatedSpec, InclusionPolicy::kReady);
    if (!status.isOK()) {
        return status;
    }

    if (resumeInfo) {
        // Don't check against unfinished indexes if this index is being resumed, since it will
        // conflict with itself.
        return validatedSpec;
    }

    // Now we will check against in-progress indexes.
    status = _doesSpecConflictWithExisting(
        opCtx, collection, validatedSpec, InclusionPolicy::kFrozen | InclusionPolicy::kUnfinished);
    if (ErrorCodes::IndexAlreadyExists == status) {
        // Callers need to be able to distinguish conflicts against ready indexes versus
        // in-progress indexes.
        return {ErrorCodes::IndexBuildAlreadyInProgress, status.reason()};
    }
    if (!status.isOK()) {
        return status;
    }

    return validatedSpec;
}

std::vector<BSONObj> IndexCatalogImpl::removeExistingIndexesNoChecks(
    OperationContext* const opCtx,
    const CollectionPtr& collection,
    const std::vector<BSONObj>& indexSpecsToBuild,
    IndexCatalog::RemoveExistingIndexesFlags flags) const {
    std::vector<BSONObj> result;
    // Filter out ready and in-progress index builds, and any non-_id indexes if 'buildIndexes' is
    // set to false in the replica set's config.
    for (const auto& spec : indexSpecsToBuild) {
        // returned to be built by the caller.
        if (ErrorCodes::OK != _isNonIDIndexAndNotAllowedToBuild(opCtx, spec)) {
            continue;
        }

        // _doesSpecConflictWithExisting currently does more work than we require here: we are only
        // interested in the index already exists error.

        auto inclusionPolicy = flags.removeInProgressIndexBuilds
            ? InclusionPolicy::kReady | InclusionPolicy::kUnfinished
            : InclusionPolicy::kReady;
        if (ErrorCodes::IndexAlreadyExists ==
            _doesSpecConflictWithExisting(
                opCtx, collection, spec, inclusionPolicy, flags.fieldsToUseForComparison)) {
            continue;
        }

        result.push_back(spec);
    }
    return result;
}

std::vector<BSONObj> IndexCatalogImpl::removeExistingIndexes(
    OperationContext* const opCtx,
    const CollectionPtr& collection,
    const std::vector<BSONObj>& indexSpecsToBuild,
    const bool removeIndexBuildsToo) const {
    std::vector<BSONObj> result;
    for (const auto& spec : indexSpecsToBuild) {
        auto prepareResult = prepareSpecForCreate(opCtx, collection, spec);
        if (prepareResult == ErrorCodes::IndexAlreadyExists ||
            (removeIndexBuildsToo && prepareResult == ErrorCodes::IndexBuildAlreadyInProgress)) {
            continue;
        }
        uassertStatusOK(prepareResult);
        result.push_back(prepareResult.getValue());
    }
    return result;
}

IndexCatalogEntry* IndexCatalogImpl::createIndexEntry(OperationContext* opCtx,
                                                      Collection* collection,
                                                      IndexDescriptor&& descriptor,
                                                      CreateIndexEntryFlags flags) {
    invariant(!descriptor.getEntry());
    dassert(!findIndexByName(opCtx, descriptor.indexName(), InclusionPolicy::kAll));

    Status status = _isSpecOk(opCtx, CollectionPtr(collection), descriptor.infoObj());
    if (!status.isOK()) {
        // If running inside a --repair operation, throw an error so the operation can attempt to
        // remove any invalid options from the index specification. Any other types of invalid index
        // specifications, e.g. not specifying a name for the index, will crash the server.
        if (storageGlobalParams.repair &&
            status.code() == ErrorCodes::InvalidIndexSpecificationOption) {
            uasserted(ErrorCodes::InvalidIndexSpecificationOption, status.reason());
        }
        if (storageGlobalParams.validate && status.code() == ErrorCodes::CannotCreateIndex) {
            uassertStatusOK(status);
        }

        LOGV2_FATAL(28782,
                    "Found an invalid index",
                    "descriptor"_attr = descriptor.infoObj(),
                    logAttrs(collection->ns()),
                    "error"_attr = redact(status));
    }

    auto engine = opCtx->getServiceContext()->getStorageEngine();
    std::string ident = engine->getMDBCatalog()->getIndexIdent(
        opCtx, collection->getCatalogId(), descriptor.indexName());

    bool isReadyIndex = CreateIndexEntryFlags::kIsReady & flags;
    bool frozen = CreateIndexEntryFlags::kFrozen & flags;
    invariant(!frozen || !isReadyIndex);

    auto entry = std::make_shared<IndexCatalogEntryImpl>(
        opCtx, CollectionPtr(collection), ident, std::move(descriptor), frozen);

    IndexDescriptor* desc = entry->descriptor();

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    // In some cases, it may be necessary to update the index metadata in the storage engine in
    // order to obtain the correct SortedDataInterface. One such scenario is found in converting an
    // index to be unique.
    bool isUpdateMetadata = CreateIndexEntryFlags::kUpdateMetadata & flags;
    if (isUpdateMetadata) {
        bool isForceUpdateMetadata = CreateIndexEntryFlags::kForceUpdateMetadata & flags;
        engine->getEngine()->alterIdentMetadata(
            ru, ident, desc->toIndexConfig(), isForceUpdateMetadata);
    }

    if (!frozen) {
        try {
            entry->setAccessMethod(IndexAccessMethod::make(opCtx,
                                                           ru,
                                                           collection->ns(),
                                                           collection->getCollectionOptions(),
                                                           entry.get(),
                                                           ident));
        } catch (const ExceptionFor<ErrorCodes::NoSuchKey>& ex) {
            // Ready indexes should always exist in the storage engine, and if they're missing
            // something has gone significantly wrong.
            if (isReadyIndex)
                throw;

            // Non-ready indexes being missing isn't normal, but isn't an error. We may have crashed
            // while creating the index and added it to the catalog but not the storage engine, or
            // while restarting an index build and have dropped the old ident but not recreated it
            // yet. If the ident was present we'd just drop and recreate it anyway.
            LOGV2(10398601,
                  "Non-frozen, non-ready index was missing entirely when loading the index "
                  "catalog. The index will be recreated by the index build process.",
                  "error"_attr = ex);
        }
    }

    IndexCatalogEntry* save = entry.get();
    if (isReadyIndex) {
        _indexUpdateIdentifier.reset();
        _readyIndexes.add(std::move(entry));
        _rebuildIndexUpdateIdentifier();
    } else if (frozen) {
        _frozenIndexes.add(std::move(entry));
    } else {
        _indexUpdateIdentifier.reset();
        _buildingIndexes.add(std::move(entry));
        _rebuildIndexUpdateIdentifier();
    }

    return save;
}

StatusWith<BSONObj> IndexCatalogImpl::createIndexOnEmptyCollection(OperationContext* opCtx,
                                                                   Collection* collection,
                                                                   BSONObj spec) {
    CollectionCatalog::get(opCtx)->invariantHasExclusiveAccessToCollection(opCtx, collection->ns());
    invariant(collection->isEmpty(opCtx),
              str::stream() << "Collection must be empty. Collection: "
                            << collection->ns().toStringForErrorMsg()
                            << " UUID: " << collection->uuid()
                            << " Count (from size storer): " << collection->numRecords(opCtx));

    StatusWith<BSONObj> statusWithSpec =

        prepareSpecForCreate(opCtx, CollectionPtr(collection), spec);
    if (!statusWithSpec.isOK())
        return statusWithSpec;

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    IndexBuildInfo indexBuildInfo(statusWithSpec.getValue(),
                                  *storageEngine,
                                  collection->ns().dbName(),
                                  VersionContext::getDecoration(opCtx));
    if (auto status = IndexBuildBlock::buildEmptyIndex(opCtx, collection, indexBuildInfo);
        !status.isOK()) {
        return status;
    }

    return statusWithSpec;
}

void IndexCatalogImpl::_rebuildIndexUpdateIdentifier() {
    const size_t numIndexes = numIndexesReady() + numIndexesInProgress();
    tassert(7639002, "numIndexes <= kMaxNumIndexesAllowed", numIndexes <= kMaxNumIndexesAllowed);
    _indexUpdateIdentifier.emplace(numIndexes);

    // Position of current index we are looking at. We are looking at all indexes from
    // '_readyIndexes' first, then '_buildingIndexes'.
    size_t indexCounter = 0;

    auto addIndexes = [&](const auto& indexesContainer) {
        for (const auto& indexEntry : indexesContainer) {
            tassert(7639003, "indexCounter < numIndexes", indexCounter < numIndexes);
            _indexUpdateIdentifier->addIndex(indexCounter, indexEntry->getIndexedPaths());
            ++indexCounter;
        }
    };

    // Build up information about ready and currently building indexes. We are intentionally not
    // including frozen indexes, because they are not updated.
    addIndexes(_readyIndexes);
    addIndexes(_buildingIndexes);
    tassert(7639004, "indexCounter == numIndexes", indexCounter == numIndexes);
}

namespace {

/**
 * Recursive function which confirms whether 'expression' is valid for use in partial indexes.
 * Recursion is restricted to 'internalPartialFilterExpressionMaxDepth' levels.
 */
Status _checkValidFilterExpressions(const MatchExpression* expression, int level) {
    if (!expression)
        return Status::OK();

    const auto kMaxDepth = internalPartialFilterExpressionMaxDepth.load();
    if ((level + 1) > kMaxDepth) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream()
                          << "partialFilterExpression depth may not exceed " << kMaxDepth);
    }

    switch (expression->matchType()) {
        case MatchExpression::AND:
            for (size_t i = 0; i < expression->numChildren(); i++) {
                Status status = _checkValidFilterExpressions(expression->getChild(i), level + 1);
                if (!status.isOK())
                    return status;
            }
            return Status::OK();

        case MatchExpression::OR:
            for (size_t i = 0; i < expression->numChildren(); i++) {
                Status status = _checkValidFilterExpressions(expression->getChild(i), level + 1);
                if (!status.isOK()) {
                    return status;
                }
            }
            return Status::OK();
        case MatchExpression::GEO:
        case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN:
        case MatchExpression::INTERNAL_EXPR_EQ:
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::INTERNAL_EXPR_LTE:
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::INTERNAL_EXPR_GTE:
        case MatchExpression::MATCH_IN:
            return Status::OK();
        case MatchExpression::EQ:
        case MatchExpression::LT:
        case MatchExpression::LTE:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::EXISTS:
        case MatchExpression::TYPE_OPERATOR:
            return Status::OK();
        default:
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "Expression not supported in partial index: "
                                        << expression->debugString());
    }
}

/**
 * Adjust the provided index spec BSONObj depending on the type of index obj describes.
 *
 * This is a no-op unless the object describes a TEXT or a GEO_2DSPHERE index.  TEXT and
 * GEO_2DSPHERE provide additional validation on the index spec, and tweak the index spec
 * object to conform to their expected format.
 */
StatusWith<BSONObj> adjustIndexSpecObject(const BSONObj& obj) {
    std::string pluginName = IndexNames::findPluginName(obj.getObjectField("key"));

    if (IndexNames::TEXT == pluginName) {
        return fts::FTSSpec::fixSpec(obj);
    }

    if (IndexNames::GEO_2DSPHERE == pluginName) {
        return S2AccessMethod::fixSpec(obj);
    }

    if (IndexNames::GEO_2DSPHERE_BUCKET == pluginName) {
        return S2BucketAccessMethod::fixSpec(obj);
    }

    return obj;
}

Status reportInvalidOption(StringData optionName, StringData pluginName) {
    return Status(ErrorCodes::CannotCreateIndex,
                  str::stream() << "Index type '" << pluginName << "' does not support the '"
                                << optionName << "' option");
}

Status reportInvalidVersion(StringData pluginName, IndexVersion indexVersion) {
    return Status(ErrorCodes::CannotCreateIndex,
                  str::stream() << "Index type '" << pluginName
                                << "' is not allowed with index version v: "
                                << static_cast<int>(indexVersion));
}

Status validateWildcardSpec(const BSONObj& spec, IndexVersion indexVersion) {
    if (spec["sparse"].trueValue())
        return reportInvalidOption("sparse", IndexNames::WILDCARD);

    if (spec["unique"].trueValue()) {
        return reportInvalidOption("unique", IndexNames::WILDCARD);
    }

    if (spec.getField("expireAfterSeconds")) {
        return reportInvalidOption("expireAfterSeconds", IndexNames::WILDCARD)
            .withContext("cannot make a TTL index");
    }
    if (indexVersion < IndexVersion::kV2) {
        return reportInvalidVersion(IndexNames::WILDCARD, indexVersion);
    }
    return Status::OK();
}
}  // namespace

Status IndexCatalogImpl::checkValidFilterExpressions(const MatchExpression* expression) {
    return _checkValidFilterExpressions(expression, 0);
}

Status IndexCatalogImpl::_isSpecOk(OperationContext* opCtx,
                                   const CollectionPtr& collection,
                                   const BSONObj& spec) const {
    const NamespaceString& nss = collection->ns();

    BSONElement vElt = spec["v"];
    if (!vElt) {
        return {ErrorCodes::InternalError,
                str::stream()
                    << "An internal operation failed to specify the 'v' field, which is a required "
                       "property of an index specification: "
                    << spec};
    }

    if (!vElt.isNumber()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "non-numeric value for \"v\" field: " << vElt);
    }

    auto vEltAsInt = representAs<int>(vElt.number());
    if (!vEltAsInt) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Index version must be representable as a 32-bit integer, but got "
                              << vElt.toString(false, false)};
    }

    auto indexVersion = static_cast<IndexVersion>(*vEltAsInt);

    if (indexVersion >= IndexVersion::kV2) {
        auto status = index_key_validate::validateIndexSpecFieldNames(spec);
        if (!status.isOK()) {
            return status;
        }
    }

    if (!IndexDescriptor::isIndexVersionSupported(indexVersion)) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "this version of mongod cannot build new indexes "
                                    << "of version number " << static_cast<int>(indexVersion));
    }

    if (nss.isOplog())
        return Status(ErrorCodes::CannotCreateIndex, "cannot have an index on the oplog");

    // logical name of the index
    const BSONElement nameElem = spec["name"];
    if (nameElem.type() != BSONType::string)
        return Status(ErrorCodes::CannotCreateIndex, "index name must be specified as a string");

    const StringData name = nameElem.valueStringData();
    if (name.find('\0') != std::string::npos)
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot contain NUL bytes");

    if (name.empty())
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot be empty");

    const BSONObj key = spec.getObjectField("key");
    const Status keyStatus = index_key_validate::validateKeyPattern(key, indexVersion);
    if (!keyStatus.isOK()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream()
                          << "bad index key pattern " << key << ": " << keyStatus.reason());
    }

    const std::string pluginName = IndexNames::findPluginName(key);
    std::unique_ptr<CollatorInterface> collator;
    BSONElement collationElement = spec.getField("collation");
    if (collationElement) {
        if (collationElement.type() != BSONType::object) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "\"collation\" for an index must be a document");
        }
        auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(collationElement.Obj());
        if (!statusWithCollator.isOK()) {
            return statusWithCollator.getStatus();
        }
        collator = std::move(statusWithCollator.getValue());

        if (!collator) {
            return {ErrorCodes::InternalError,
                    str::stream() << "An internal operation specified the collation "
                                  << CollationSpec::kSimpleSpec
                                  << " explicitly, which should instead be implied by omitting the "
                                     "'collation' field from the index specification"};
        }

        if (static_cast<IndexVersion>(vElt.numberInt()) < IndexVersion::kV2) {
            return {ErrorCodes::CannotCreateIndex,
                    str::stream() << "Index version " << vElt.fieldNameStringData() << "="
                                  << vElt.numberInt() << " does not support the '"
                                  << collationElement.fieldNameStringData() << "' option"};
        }

        if ((pluginName != IndexNames::BTREE) && (pluginName != IndexNames::GEO_2DSPHERE) &&
            (pluginName != IndexNames::HASHED) && (pluginName != IndexNames::WILDCARD)) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream()
                              << "Index type '" << pluginName
                              << "' does not support collation: " << collator->getSpec().toBSON());
        }
    }

    const bool isSparse = spec["sparse"].trueValue();

    if (pluginName == IndexNames::WILDCARD) {
        if (auto wildcardSpecStatus = validateWildcardSpec(spec, indexVersion);
            !wildcardSpecStatus.isOK()) {
            return wildcardSpecStatus;
        }
    } else if (pluginName == IndexNames::COLUMN) {
        uasserted(ErrorCodes::NotImplemented, "Columnstore indexes can no longer be created");
    }

    // Create an ExpressionContext, used to parse the match expression and to house the collator for
    // the remaining checks.
    auto expCtx =
        ExpressionContextBuilder{}.opCtx(opCtx).collator(std::move(collator)).ns(nss).build();
    // Ensure if there is a filter, its valid.
    BSONElement filterElement = spec.getField("partialFilterExpression");
    if (filterElement) {
        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "cannot mix \"partialFilterExpression\" and \"sparse\" options");
        }

        if (filterElement.type() != BSONType::object) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "\"partialFilterExpression\" for an index must be a document");
        }

        // Parsing the partial filter expression is not expected to fail here since the
        // expression would have been successfully parsed upstream during index creation.
        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(filterElement.Obj(),
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kBanAllSpecialFeatures);
        if (!statusWithMatcher.isOK()) {
            return statusWithMatcher.getStatus();
        }
        const std::unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        Status status = checkValidFilterExpressions(filterExpr.get());
        if (!status.isOK()) {
            return status;
        }
    }

    BSONElement clusteredElt = spec["clustered"];
    if (collection->isClustered() || (clusteredElt && clusteredElt.trueValue())) {
        // Clustered collections require checks to ensure the spec does not conflict with the
        // implicit clustered index that exists on the clustered collection.
        auto status = isSpecOKClusteredIndexCheck(spec, collection->getClusteredInfo());
        if (!status.isOK()) {
            return status;
        }
    }

    if (IndexDescriptor::isIdIndexPattern(key)) {
        if (collection->isClustered() &&
            !clustered_util::matchesClusterKey(key, collection->getClusteredInfo())) {
            return Status(
                ErrorCodes::CannotCreateIndex,
                "cannot create the _id index on a clustered collection not clustered by _id");
        }

        BSONElement uniqueElt = spec["unique"];
        if (uniqueElt && !uniqueElt.trueValue()) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be non-unique");
        }

        if (filterElement) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be a partial index");
        }

        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be sparse");
        }

        if (collationElement &&
            !CollatorInterface::collatorsMatch(expCtx->getCollator(),
                                               collection->getDefaultCollator())) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "_id index must have the collection default collation");
        }
    }

    // --- only storage engine checks allowed below this ----

    BSONElement storageEngineElement = spec.getField("storageEngine");
    if (storageEngineElement.eoo()) {
        return Status::OK();
    }
    if (storageEngineElement.type() != BSONType::object) {
        return Status(ErrorCodes::CannotCreateIndex,
                      "\"storageEngine\" options must be a document if present");
    }
    BSONObj storageEngineOptions = storageEngineElement.Obj();
    if (storageEngineOptions.isEmpty()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      "Empty \"storageEngine\" options are invalid. "
                      "Please remove the field or include valid options.");
    }
    Status storageEngineStatus = validateStorageOptions(
        opCtx->getServiceContext(), storageEngineOptions, [](const auto& x, const auto& y) {
            return x->validateIndexStorageOptions(y);
        });
    if (!storageEngineStatus.isOK()) {
        return storageEngineStatus;
    }

    return Status::OK();
}

Status IndexCatalogImpl::_doesSpecConflictWithExisting(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const BSONObj& spec,
    InclusionPolicy inclusionPolicy,
    const std::map<StringData, std::set<IndexType>>* allowedFieldNames) const {
    StringData name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
    invariant(name[0]);

    const BSONObj key = spec.getObjectField(IndexDescriptor::kKeyPatternFieldName);

    if (spec["clustered"]) {
        // Not an error, but the spec is already validated against the collection options by
        // _isSpecOK now and we know that if 'clustered' is true, then the index already exists.
        return Status(ErrorCodes::IndexAlreadyExists, "The clustered index is implicitly built");
    }

    {
        // Check whether an index with the specified candidate name already exists in the catalog.
        const IndexDescriptor* desc = findIndexByName(opCtx, name, inclusionPolicy);

        if (desc) {
            // Index already exists with same name. Check whether the options are the same as well.
            auto entry = getEntry(desc);
            IndexDescriptor candidate(_getAccessMethodName(key), spec);
            auto indexComparison = [&] {
                if (allowedFieldNames) {
                    auto filteredEntry = entry->cloneWithDifferentDescriptor(
                        IndexDescriptor(desc->getAccessMethodName(),
                                        index_key_validate::repairIndexSpec(
                                            collection->ns(), desc->toBSON(), *allowedFieldNames)));
                    return candidate.compareIndexOptions(
                        opCtx, collection->ns(), filteredEntry.get());
                } else {
                    return candidate.compareIndexOptions(opCtx, collection->ns(), entry);
                }
            }();

            // Key pattern or another uniquely-identifying option differs. We can build this index,
            // but not with the specified (duplicate) name. User must specify another index name.
            if (indexComparison == IndexDescriptor::Comparison::kDifferent) {
                return Status(ErrorCodes::IndexKeySpecsConflict,
                              str::stream()
                                  << "An existing index has the same name as the "
                                     "requested index. When index names are not specified, they "
                                     "are auto generated and can cause conflicts. Please refer to "
                                     "our documentation. Requested index: "
                                  << spec << ", existing index: " << desc->infoObj());
            }

            // The candidate's key and uniquely-identifying options are equivalent to an existing
            // index, but some other options are not identical. Return a message to that effect.
            if (indexComparison == IndexDescriptor::Comparison::kEquivalent) {
                return Status(ErrorCodes::IndexOptionsConflict,
                              str::stream() << "An equivalent index already exists with the same "
                                               "name but different options. Requested index: "
                                            << spec << ", existing index: " << desc->infoObj());
            }

            // If we've reached this point, the requested index is identical to an existing index.
            invariant(indexComparison == IndexDescriptor::Comparison::kIdentical);

            // If an identical index exists, but it is frozen, return an error with a different
            // error code to the user, forcing the user to drop before recreating the index.
            if (entry->isFrozen()) {
                return Status(ErrorCodes::CannotCreateIndex,
                              str::stream()
                                  << "An identical, unfinished index '" << name
                                  << "' already exists. Must drop before recreating. Spec: "
                                  << desc->infoObj());
            }

            // Index already exists with the same options, so there is no need to build a new one.
            // This is not an error condition.
            return Status(ErrorCodes::IndexAlreadyExists,
                          str::stream() << "Identical index already exists: " << name);
        }
    }

    {
        // No index with the candidate name exists. Check for an index with conflicting options.
        const IndexDescriptor* desc =
            findIndexByKeyPatternAndOptions(opCtx, key, spec, inclusionPolicy);

        if (desc) {
            LOGV2_DEBUG(20353,
                        2,
                        "Index already exists with a different name",
                        "name"_attr = desc->indexName(),
                        "spec"_attr = desc->infoObj());

            // Index already exists with a different name. Check whether the options are identical.
            // We will return an error in either case, but this check allows us to generate a more
            // informative error message.
            auto entry = getEntry(desc);

            IndexDescriptor candidate(_getAccessMethodName(key), spec);
            auto indexComparison = [&] {
                if (allowedFieldNames) {
                    auto filteredEntry = entry->cloneWithDifferentDescriptor(
                        IndexDescriptor(desc->getAccessMethodName(),
                                        index_key_validate::repairIndexSpec(
                                            collection->ns(), desc->toBSON(), *allowedFieldNames)));
                    return candidate.compareIndexOptions(
                        opCtx, collection->ns(), filteredEntry.get());
                } else {
                    return candidate.compareIndexOptions(opCtx, collection->ns(), entry);
                }
            }();

            // The candidate's key and uniquely-identifying options are equivalent to an existing
            // index, but some other options are not identical. Return a message to that effect.
            if (indexComparison == IndexDescriptor::Comparison::kEquivalent)
                return Status(ErrorCodes::IndexOptionsConflict,
                              str::stream() << "An equivalent index already exists with a "
                                               "different name and options. Requested index: "
                                            << spec << ", existing index: " << desc->infoObj());

            // If we've reached this point, the requested index is identical to an existing index.
            invariant(indexComparison == IndexDescriptor::Comparison::kIdentical);

            // An identical index already exists with a different name. We cannot build this index.
            return Status(ErrorCodes::IndexOptionsConflict,
                          str::stream() << "Index already exists with a different name: "
                                        << desc->indexName());
        }
    }

    if (numIndexesTotal() >= kMaxNumIndexesAllowed) {
        std::string s = str::stream() << "add index fails, too many indexes for "
                                      << collection->ns().toStringForErrorMsg() << " key:" << key;
        LOGV2(20354,
              "Exceeded maximum number of indexes",
              logAttrs(collection->ns()),
              "key"_attr = key,
              "maxNumIndexes"_attr = kMaxNumIndexesAllowed);
        return Status(ErrorCodes::CannotCreateIndex, s);
    }

    // Refuse to build text index if another text index exists or is in progress.
    // Collections should only have one text index.
    std::string pluginName = IndexNames::findPluginName(key);
    if (pluginName == IndexNames::TEXT) {
        std::vector<const IndexDescriptor*> textIndexes;
        findIndexByType(opCtx, IndexNames::TEXT, textIndexes, inclusionPolicy);
        if (textIndexes.size() > 0) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "only one text index per collection allowed, "
                                        << "found existing text index \""
                                        << textIndexes[0]->indexName() << "\"");
        }
    }
    return Status::OK();
}

BSONObj IndexCatalogImpl::getDefaultIdIndexSpec(const CollectionPtr& collection) const {
    dassert(_idObj["_id"].type() == BSONType::numberInt);

    const auto indexVersion = IndexDescriptor::getDefaultIndexVersion();

    BSONObjBuilder b;
    b.append("v", static_cast<int>(indexVersion));
    b.append("name", IndexConstants::kIdIndexName);
    b.append("key", _idObj);
    if (collection->getDefaultCollator() && indexVersion >= IndexVersion::kV2) {
        // Creating an index with the "collation" option requires a v=2 index.
        b.append("collation", collection->getDefaultCollator()->getSpec().toBSON());
    }
    return b.obj();
}

void IndexCatalogImpl::dropIndexes(OperationContext* opCtx,
                                   Collection* collection,
                                   std::function<bool(const IndexDescriptor*)> matchFn,
                                   std::function<void(const IndexDescriptor*)> onDropFn) {
    uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            str::stream() << "cannot perform operation: an index build is currently running",
            !haveAnyIndexesInProgress());

    bool didExclude = false;

    invariant(_buildingIndexes.size() == 0);
    std::vector<std::string> indexNamesToDrop;
    {
        int seen = 0;
        auto ii = getIndexIterator(InclusionPolicy::kAll);
        while (ii->more()) {
            seen++;
            const IndexDescriptor* desc = ii->next()->descriptor();
            if (matchFn(desc)) {
                indexNamesToDrop.push_back(desc->indexName());
            } else {
                didExclude = true;
            }
        }
        invariant(seen == numIndexesTotal());
    }

    for (const std::string& indexName : indexNamesToDrop) {
        IndexCatalogEntry* writableEntry =
            getWritableEntryByName(opCtx, indexName, InclusionPolicy::kAll);
        invariant(writableEntry);
        LOGV2_DEBUG(20355,
                    1,
                    "\t dropAllIndexes dropping: {desc}",
                    "desc"_attr = *writableEntry->descriptor());


        // If the onDrop function creates an oplog entry, it should run first so that the drop is
        // timestamped at the same optime.
        if (onDropFn) {
            onDropFn(writableEntry->descriptor());
        }
        invariant(dropIndexEntry(opCtx, collection, writableEntry));
    }

    // verify state is sane post cleaning

    long long numIndexesInCollectionCatalogEntry = collection->getTotalIndexCount();

    if (!didExclude) {
        if (numIndexesTotal() || numIndexesInCollectionCatalogEntry || _readyIndexes.size()) {
            _logInternalState(opCtx,
                              CollectionPtr(collection),
                              numIndexesInCollectionCatalogEntry,
                              indexNamesToDrop);
        }
        fassert(17327, numIndexesTotal() == 0);
        fassert(17328, numIndexesInCollectionCatalogEntry == 0);
        fassert(17337, _readyIndexes.size() == 0);
    }
}

void IndexCatalogImpl::dropAllIndexes(OperationContext* opCtx,
                                      Collection* collection,
                                      bool includingIdIndex,
                                      std::function<void(const IndexDescriptor*)> onDropFn) {
    dropIndexes(
        opCtx,
        collection,
        [includingIdIndex](const IndexDescriptor* indexDescriptor) {
            if (includingIdIndex) {
                return true;
            }

            return !indexDescriptor->isIdIndex();
        },
        onDropFn);
}

Status IndexCatalogImpl::truncateAllIndexes(OperationContext* opCtx, Collection* collection) {
    uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            str::stream() << "cannot perform operation: an index build is currently running",
            !haveAnyIndexesInProgress());
    invariant(_buildingIndexes.size() == 0);

    auto it = getIndexIterator(IndexCatalog::InclusionPolicy::kAll);
    while (it->more()) {
        const IndexDescriptor* desc = it->next()->descriptor();
        auto status = desc->getEntry()->accessMethod()->truncate(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx));
        if (!status.isOK())
            return status;
    }
    return Status::OK();
}

void IndexCatalogImpl::_removeEntry(IndexCatalogEntry* entry) {
    auto desc = entry->descriptor();
    if (kDebugBuild) {
        auto check = [&](auto& container) {
            return std::count_if(container.begin(), container.end(), [&](auto&& entry) {
                return entry->descriptor() == desc ||
                    entry->descriptor()->indexName() == desc->indexName();
            });
        };
        auto count = check(_readyIndexes) + check(_buildingIndexes) + check(_frozenIndexes);
        invariant(count == 1);
    }

    if (!_readyIndexes.remove(desc) && !_buildingIndexes.remove(desc) &&
        !_frozenIndexes.remove(desc)) {
        MONGO_UNREACHABLE_TASSERT(10083505);
    }
}

Status IndexCatalogImpl::resetUnfinishedIndexForRecovery(OperationContext* opCtx,
                                                         Collection* collection,
                                                         IndexCatalogEntry* entry) {
    invariant(
        shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(collection->ns(), MODE_X));
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    auto ownedEntry = entry->shared_from_this();
    const std::string indexName = entry->descriptor()->indexName();

    // Only indexes that aren't ready can be reset.
    invariant(!collection->isIndexReady(indexName));
    _removeEntry(entry);

    LOGV2(6987700,
          "Resetting unfinished index",
          logAttrs(collection->ns()),
          "index"_attr = indexName,
          "ident"_attr = entry->getIdent());


    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    // Drop the ident if it exists. The storage engine will return OK if the ident is not found.
    auto engine = opCtx->getServiceContext()->getStorageEngine();
    const std::string ident = entry->getIdent();
    Status status = engine->getEngine()->dropIdent(ru, ident, /*identHasSizeInfo=*/false);
    if (!status.isOK()) {
        return status;
    }

    // Recreate the ident on-disk.
    const auto indexDescriptor = entry->descriptor();
    status = durable_catalog::createIndex(opCtx,
                                          collection->getCatalogId(),
                                          collection->ns(),
                                          collection->getCollectionOptions(),
                                          indexDescriptor->toIndexConfig(),
                                          ident);
    if (!status.isOK()) {
        return status;
    }

    // Update the index entry state in preparation to rebuild the index.
    if (!entry->accessMethod()) {
        entry->setAccessMethod(IndexAccessMethod::make(
            opCtx, ru, collection->ns(), collection->getCollectionOptions(), entry, ident));
    }

    entry->setIsFrozen(false);
    _buildingIndexes.add(std::move(ownedEntry));

    return Status::OK();
}

Status IndexCatalogImpl::dropUnfinishedIndex(OperationContext* opCtx,
                                             Collection* collection,
                                             IndexCatalogEntry* entry) {
    if (!entry)
        return Status(ErrorCodes::InternalError, "cannot find index to delete");

    if (entry->isReady())
        return Status(ErrorCodes::InternalError, "expected unfinished index, but it is ready");

    return dropIndexEntry(opCtx, collection, entry);
}

Status IndexCatalogImpl::dropIndexEntry(OperationContext* opCtx,
                                        Collection* collection,
                                        IndexCatalogEntry* entry) {
    invariant(entry);

    // Pulling indexName out as it is needed post descriptor release.
    std::string indexName = entry->descriptor()->indexName();

    audit::logDropIndex(opCtx->getClient(), indexName, collection->ns());

    auto ownedEntry = entry->shared_from_this();
    _indexUpdateIdentifier.reset();
    _removeEntry(entry);

    _rebuildIndexUpdateIdentifier();

    CollectionQueryInfo::get(collection).rebuildIndexData(opCtx, collection);
    CollectionIndexUsageTrackerDecoration::write(collection).unregisterIndex(indexName);
    _deleteIndexFromDisk(opCtx, collection, indexName, std::move(ownedEntry));

    return Status::OK();
}

void IndexCatalogImpl::deleteIndexFromDisk(OperationContext* opCtx,
                                           Collection* collection,
                                           const std::string& indexName) {
    _deleteIndexFromDisk(opCtx, collection, indexName, nullptr);
}

void IndexCatalogImpl::_deleteIndexFromDisk(OperationContext* opCtx,
                                            Collection* collection,
                                            const std::string& indexName,
                                            std::shared_ptr<IndexCatalogEntry> entry) {
    invariant(!findIndexByName(opCtx, indexName, InclusionPolicy::kAll));

    catalog::DataRemoval dataRemoval = catalog::DataRemoval::kTwoPhase;
    if (!entry || !entry->getSharedIdent()) {
        // getSharedIdent() returns a nullptr for unfinished index builds. These indexes can be
        // removed immediately as they weren't ready for use yet.
        dataRemoval = catalog::DataRemoval::kImmediate;
    }
    catalog::removeIndex(opCtx, indexName, collection, std::move(entry), dataRemoval);
}

void IndexCatalogImpl::setMultikeyPaths(OperationContext* const opCtx,
                                        const CollectionPtr& coll,
                                        const IndexDescriptor* desc,
                                        const KeyStringSet& multikeyMetadataKeys,
                                        const MultikeyPaths& multikeyPaths) const {
    const IndexCatalogEntry* entry = desc->getEntry();
    invariant(entry);
    entry->setMultikey(opCtx, coll, multikeyMetadataKeys, multikeyPaths);
};

// ---------------------------

bool IndexCatalogImpl::haveAnyIndexes() const {
    return _readyIndexes.size() > 0 || _buildingIndexes.size() > 0;
}

bool IndexCatalogImpl::haveAnyIndexesInProgress() const {
    return _buildingIndexes.size() > 0;
}

int IndexCatalogImpl::numIndexesTotal() const {
    return _readyIndexes.size() + _buildingIndexes.size() + _frozenIndexes.size();
}

int IndexCatalogImpl::numIndexesReady() const {
    return _readyIndexes.size();
}

int IndexCatalogImpl::numIndexesInProgress() const {
    return _buildingIndexes.size();
}

bool IndexCatalogImpl::haveIdIndex(OperationContext* opCtx) const {
    return findIdIndex(opCtx) != nullptr;
}

const IndexDescriptor* IndexCatalogImpl::findIdIndex(OperationContext* opCtx) const {
    return _readyIndexes.getIdIndex();
}

const IndexDescriptor* IndexCatalogImpl::findIndexByName(OperationContext* opCtx,
                                                         StringData name,
                                                         InclusionPolicy inclusionPolicy) const {
    auto ii = getIndexIterator(inclusionPolicy);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (desc->indexName() == name)
            return desc;
    }
    return nullptr;
}

const IndexDescriptor* IndexCatalogImpl::findIndexByKeyPatternAndOptions(
    OperationContext* opCtx,
    const BSONObj& key,
    const BSONObj& indexSpec,
    InclusionPolicy inclusionPolicy) const {
    auto ii = getIndexIterator(inclusionPolicy);
    IndexDescriptor needle(_getAccessMethodName(key), indexSpec);
    while (ii->more()) {
        const auto* entry = ii->next();
        if (needle.compareIndexOptions(opCtx, {}, entry) !=
            IndexDescriptor::Comparison::kDifferent) {
            return entry->descriptor();
        }
    }
    return nullptr;
}  // namespace mongo

void IndexCatalogImpl::findIndexesByKeyPattern(OperationContext* opCtx,
                                               const BSONObj& key,
                                               InclusionPolicy inclusionPolicy,
                                               std::vector<const IndexDescriptor*>* matches) const {
    invariant(matches);
    auto ii = getIndexIterator(inclusionPolicy);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (SimpleBSONObjComparator::kInstance.evaluate(desc->keyPattern() == key)) {
            matches->push_back(desc);
        }
    }
}

void IndexCatalogImpl::findIndexByType(OperationContext* opCtx,
                                       const std::string& type,
                                       std::vector<const IndexDescriptor*>& matches,
                                       InclusionPolicy inclusionPolicy) const {
    auto ii = getIndexIterator(inclusionPolicy);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (IndexNames::findPluginName(desc->keyPattern()) == type) {
            matches.push_back(desc);
        }
    }
}

const IndexDescriptor* IndexCatalogImpl::findIndexByIdent(OperationContext* opCtx,
                                                          StringData ident,
                                                          InclusionPolicy inclusionPolicy) const {
    auto ii = getIndexIterator(inclusionPolicy);
    while (ii->more()) {
        const IndexCatalogEntry* entry = ii->next();
        if (ident == entry->getIdent()) {
            return entry->descriptor();
        }
    }
    return nullptr;
}

const IndexCatalogEntry* IndexCatalogImpl::getEntry(const IndexDescriptor* desc) const {
    const IndexCatalogEntry* entry = desc->getEntry();
    massert(17357, "cannot find index entry", entry);
    return entry;
}

IndexCatalogEntry* IndexCatalogImpl::getWritableEntryByName(OperationContext* opCtx,
                                                            StringData name,
                                                            InclusionPolicy inclusionPolicy) {
    return _getWritableEntry(findIndexByName(opCtx, name, inclusionPolicy));
}

IndexCatalogEntry* IndexCatalogImpl::getWritableEntryByKeyPatternAndOptions(
    OperationContext* opCtx,
    const BSONObj& key,
    const BSONObj& indexSpec,
    InclusionPolicy inclusionPolicy) {
    return _getWritableEntry(
        findIndexByKeyPatternAndOptions(opCtx, key, indexSpec, inclusionPolicy));
}

IndexCatalogEntry* IndexCatalogImpl::_getWritableEntry(const IndexDescriptor* descriptor) {
    if (!descriptor) {
        return nullptr;
    }

    auto getWritableEntry = [&](auto& container) -> IndexCatalogEntry* {
        std::shared_ptr<const IndexCatalogEntry> oldEntry = container.release(descriptor);
        invariant(oldEntry);

        // This collection instance already uniquely owns this IndexCatalogEntry, return it.
        if (oldEntry.use_count() == 1) {
            IndexCatalogEntry* entryToReturn = const_cast<IndexCatalogEntry*>(oldEntry.get());
            container.add(std::move(oldEntry));
            return entryToReturn;
        }

        std::shared_ptr<IndexCatalogEntryImpl> writableEntry =
            std::make_shared<IndexCatalogEntryImpl>(
                *static_cast<const IndexCatalogEntryImpl*>(oldEntry.get()));
        writableEntry->descriptor()->setEntry(writableEntry.get());
        IndexCatalogEntry* entryToReturn = writableEntry.get();
        container.add(std::move(writableEntry));
        return entryToReturn;
    };

    if (descriptor->getEntry()->isReady()) {
        _indexUpdateIdentifier.reset();
        IndexCatalogEntry* result = getWritableEntry(_readyIndexes);
        _rebuildIndexUpdateIdentifier();
        return result;
    } else if (!descriptor->getEntry()->isFrozen()) {
        _indexUpdateIdentifier.reset();
        IndexCatalogEntry* result = getWritableEntry(_buildingIndexes);
        _rebuildIndexUpdateIdentifier();
        return result;
    } else {
        return getWritableEntry(_frozenIndexes);
    }
}

std::shared_ptr<const IndexCatalogEntry> IndexCatalogImpl::getEntryShared(
    const IndexDescriptor* indexDescriptor) const {
    return indexDescriptor->getEntry()->shared_from_this();
}

std::vector<std::shared_ptr<const IndexCatalogEntry>> IndexCatalogImpl::getAllReadyEntriesShared()
    const {
    return {_readyIndexes.begin(), _readyIndexes.end()};
}

const IndexDescriptor* IndexCatalogImpl::refreshEntry(OperationContext* opCtx,
                                                      Collection* collection,
                                                      const IndexDescriptor* oldDesc,
                                                      CreateIndexEntryFlags flags) {
    invariant(_buildingIndexes.size() == 0);

    const std::string indexName = oldDesc->indexName();
    invariant(collection->isIndexReady(indexName));

    // Delete the IndexCatalogEntry that owns this descriptor. After deletion, 'oldDesc' is invalid
    // and should not be dereferenced. Also, invalidate the index from the
    // CollectionIndexUsageTrackerDecoration (shared state among Collection instances).
    IndexCatalogEntry* writableEntry = _getWritableEntry(oldDesc);
    invariant(writableEntry);
    std::shared_ptr<const IndexCatalogEntry> deletedEntry =
        _readyIndexes.release(writableEntry->descriptor());
    invariant(writableEntry == deletedEntry.get());

    auto& indexUsageTracker = CollectionIndexUsageTrackerDecoration::write(collection);
    indexUsageTracker.unregisterIndex(indexName);

    // Ask the CollectionCatalogEntry for the new index spec.
    BSONObj spec = collection->getIndexSpec(indexName).getOwned();
    BSONObj keyPattern = spec.getObjectField("key");

    // Re-register this index in the index catalog with the new spec. Also, add the new index
    // to the CollectionIndexUsageTrackerDecoration (shared state among Collection instances).
    auto newDesc = IndexDescriptor(_getAccessMethodName(keyPattern), spec);
    auto newEntry = createIndexEntry(opCtx, collection, std::move(newDesc), flags);
    invariant(newEntry->isReady());
    auto desc = newEntry->descriptor();
    indexUsageTracker.registerIndex(desc->indexName(),
                                    desc->keyPattern(),
                                    IndexFeatures::make(desc, collection->ns().isOnInternalDb()));

    // Last rebuild index data for CollectionQueryInfo for this Collection.
    CollectionQueryInfo::get(collection).rebuildIndexData(opCtx, collection);

    // Return the new descriptor.
    return newEntry->descriptor();
}

// ---------------------------


Status IndexCatalogImpl::_indexFilteredRecords(OperationContext* opCtx,
                                               const CollectionPtr& coll,
                                               const IndexCatalogEntry* index,
                                               const std::vector<BsonRecord>& bsonRecords,
                                               int64_t* keysInsertedOut) const {
    SharedBufferFragmentBuilder pooledBuilder(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

    InsertDeleteOptions options;
    prepareInsertDeleteOptions(opCtx, coll->ns(), index->descriptor(), &options);

    return index->accessMethod()->insert(
        opCtx, pooledBuilder, coll, index, bsonRecords, options, keysInsertedOut);
}

Status IndexCatalogImpl::_indexRecords(OperationContext* opCtx,
                                       const CollectionPtr& coll,
                                       const IndexCatalogEntry* index,
                                       const std::vector<BsonRecord>& bsonRecords,
                                       int64_t* keysInsertedOut) const {
    bool skip = false;
    skipIndexNewRecords.execute([&](const BSONObj& dataObj) {
        // Skip the _id index by default.
        if (!dataObj.hasField("skipIdIndex") || dataObj["skipIdIndex"].Bool() == true) {
            skip = true;
        } else if (!index->descriptor()->isIdIndex()) {
            skip = true;
        }
    });
    if (skip)
        return Status::OK();

    const MatchExpression* filter = index->getFilterExpression();
    if (!filter)
        return _indexFilteredRecords(opCtx, coll, index, bsonRecords, keysInsertedOut);

    std::vector<BsonRecord> filteredBsonRecords;
    for (const auto& bsonRecord : bsonRecords) {
        if (exec::matcher::matchesBSON(filter, *(bsonRecord.docPtr)))
            filteredBsonRecords.push_back(bsonRecord);
    }

    return _indexFilteredRecords(opCtx, coll, index, filteredBsonRecords, keysInsertedOut);
}

Status IndexCatalogImpl::_updateRecord(OperationContext* const opCtx,
                                       const CollectionPtr& coll,
                                       const IndexCatalogEntry* index,
                                       const BSONObj& oldDoc,
                                       const BSONObj& newDoc,
                                       const RecordId& recordId,
                                       int64_t* const keysInsertedOut,
                                       int64_t* const keysDeletedOut) const {
    // TODO SERVER-80257: This failpoint was added to produce index corruption scenarios where an
    // index has incorrect keys. Replace this failpoint with a test command instead.
    if (auto failpoint = skipUpdatingIndexDocument.scoped(); MONGO_unlikely(failpoint.isActive()) &&
        repl::feature_flags::gSecondaryIndexChecksInDbCheck.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        auto indexName = failpoint.getData()["indexName"].valueStringDataSafe();
        if (indexName == index->descriptor()->indexName()) {
            LOGV2_DEBUG(
                7844805,
                3,
                "Skipping updating index record because failpoint skipUpdatingIndexDocument is on",
                "indexName"_attr = indexName);
            return Status::OK();
        }
    }
    SharedBufferFragmentBuilder pooledBuilder(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

    InsertDeleteOptions options;
    prepareInsertDeleteOptions(opCtx, coll->ns(), index->descriptor(), &options);

    int64_t keysInserted = 0;
    int64_t keysDeleted = 0;

    auto status = index->accessMethod()->update(opCtx,
                                                *shard_role_details::getRecoveryUnit(opCtx),
                                                pooledBuilder,
                                                oldDoc,
                                                newDoc,
                                                recordId,
                                                coll,
                                                index,
                                                options,
                                                &keysInserted,
                                                &keysDeleted);

    if (!status.isOK())
        return status;

    *keysInsertedOut += keysInserted;
    *keysDeletedOut += keysDeleted;

    return Status::OK();
}

void IndexCatalogImpl::_unindexRecord(OperationContext* opCtx,
                                      const CollectionPtr& collection,
                                      const IndexCatalogEntry* entry,
                                      const BSONObj& obj,
                                      const RecordId& loc,
                                      bool logIfError,
                                      int64_t* keysDeletedOut,
                                      CheckRecordId checkRecordId) const {
    // Tests can enable this failpoint to produce index corruption scenarios where an index has
    // extra keys.
    if (auto failpoint = skipUnindexingDocumentWhenDeleted.scoped();
        MONGO_unlikely(failpoint.isActive())) {
        auto indexName = failpoint.getData()["indexName"].valueStringDataSafe();
        if (indexName == entry->descriptor()->indexName()) {
            LOGV2_DEBUG(
                7844806,
                3,
                "Skipping unindexing document because failpoint skipUnindexingDocumentWhenDeleted "
                "is on",
                "indexName"_attr = indexName);
            return;
        }
    }

    SharedBufferFragmentBuilder pooledBuilder(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

    InsertDeleteOptions options;
    prepareInsertDeleteOptions(opCtx, collection->ns(), entry->descriptor(), &options);

    entry->accessMethod()->remove(opCtx,
                                  pooledBuilder,
                                  collection,
                                  entry,
                                  obj,
                                  loc,
                                  logIfError,
                                  options,
                                  keysDeletedOut,
                                  checkRecordId);
}

Status IndexCatalogImpl::indexRecords(OperationContext* opCtx,
                                      const CollectionPtr& coll,
                                      const std::vector<BsonRecord>& bsonRecords,
                                      int64_t* keysInsertedOut) const {
    if (keysInsertedOut) {
        *keysInsertedOut = 0;
    }

    // For vectored inserts, we insert index keys and flip multikey in "index order". However
    // because multikey state for different indexes both live on the same _mdb_catalog document,
    // index order isn't necessarily timestamp order. We track multikey paths here to ensure we make
    // changes to the _mdb_catalog document with in timestamp order updates.
    MultikeyPathTracker& tracker = MultikeyPathTracker::get(opCtx);

    // Take care when choosing to aggregate multikey writes. This code will only* track multikey
    // when:
    // * No parent is tracking multikey and*
    // * There are timestamps associated with the input `bsonRecords`.
    //
    // If we are not responsible for tracking multikey:
    // * Leave the multikey tracker in its original "tracking" state.
    // * Not write any accumulated multikey paths to the _mdb_catalog document.
    const bool manageMultikeyWrite =
        !tracker.isTrackingMultikeyPathInfo() && !bsonRecords[0].ts.isNull();

    ON_BLOCK_EXIT([&] {
        if (manageMultikeyWrite) {
            tracker.clear();
        }
    });

    {
        ScopeGuard stopTrackingMultikeyChanges(
            [&tracker] { tracker.stopTrackingMultikeyPathInfo(); });
        if (manageMultikeyWrite) {
            invariant(tracker.isEmpty());
            tracker.startTrackingMultikeyPathInfo();
        } else {
            stopTrackingMultikeyChanges.dismiss();
        }
        for (auto&& it : _readyIndexes) {
            Status s = _indexRecords(opCtx, coll, it.get(), bsonRecords, keysInsertedOut);
            if (!s.isOK())
                return s;
        }

        for (auto&& it : _buildingIndexes) {
            Status s = _indexRecords(opCtx, coll, it.get(), bsonRecords, keysInsertedOut);
            if (!s.isOK())
                return s;
        }
    }

    const WorkerMultikeyPathInfo& newPaths = tracker.getMultikeyPathInfo();
    if (newPaths.size() == 0 || !manageMultikeyWrite) {
        return Status::OK();
    }

    if (Status status = shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(bsonRecords[0].ts);
        !status.isOK()) {
        return status;
    }

    for (const MultikeyPathInfo& newPath : newPaths) {
        invariant(newPath.nss == coll->ns());
        auto idx = findIndexByName(
            opCtx, newPath.indexName, InclusionPolicy::kReady | InclusionPolicy::kUnfinished);
        if (!idx) {
            return Status(ErrorCodes::IndexNotFound,
                          str::stream() << "Could not find index " << newPath.indexName << " in "
                                        << coll->ns().toStringForErrorMsg() << " (" << coll->uuid()
                                        << ") to set to multikey.");
        }
        setMultikeyPaths(opCtx, coll, idx, newPath.multikeyMetadataKeys, newPath.multikeyPaths);
    }

    return Status::OK();
}

Status IndexCatalogImpl::updateRecord(OperationContext* const opCtx,
                                      const CollectionPtr& coll,
                                      const BSONObj& oldDoc,
                                      const BSONObj& newDoc,
                                      const BSONObj* opDiff,
                                      const RecordId& recordId,
                                      int64_t* const keysInsertedOut,
                                      int64_t* const keysDeletedOut) const {
    *keysInsertedOut = 0;
    *keysDeletedOut = 0;

    Status status = Status::OK();

    if (size_t numIndexesToUpdate = _readyIndexes.size() + _buildingIndexes.size();
        numIndexesToUpdate > 0) {
        // Fetch index from the catalog by position. We return indexes from _readyIndexes, then
        // _buildingIndexes.
        auto indexAtPosition = [&](size_t pos) -> const IndexCatalogEntry* {
            dassert(pos < numIndexesToUpdate);
            return (pos < _readyIndexes.size())
                ? (_readyIndexes.begin() + pos)->get()
                : (_buildingIndexes.begin() + (pos - _readyIndexes.size()))->get();
        };

        if (opDiff) {
            // Update only affected indexes. Build an IndexSet with all indexes that may be affected
            // by the update.
            tassert(7639005,
                    "_indexUpdateIdentifier must have a value",
                    _indexUpdateIdentifier.has_value());

            const mongo::doc_diff::IndexSet indexSetToUpdate =
                _indexUpdateIdentifier->determineAffectedIndexes(*opDiff);

            for (size_t pos = indexSetToUpdate.findFirst(); pos != mongo::doc_diff::IndexSet::npos;
                 pos = indexSetToUpdate.findNext(pos)) {
                status = _updateRecord(opCtx,
                                       coll,
                                       indexAtPosition(pos),
                                       oldDoc,
                                       newDoc,
                                       recordId,
                                       keysInsertedOut,
                                       keysDeletedOut);
                if (!status.isOK()) {
                    break;
                }
            }
        } else {
            // Update all indexes. Simply iterate over all ready and in progress indexes here
            // without building an intermediate IndexSet.
            for (size_t pos = 0; pos < numIndexesToUpdate; pos++) {
                status = _updateRecord(opCtx,
                                       coll,
                                       indexAtPosition(pos),
                                       oldDoc,
                                       newDoc,
                                       recordId,
                                       keysInsertedOut,
                                       keysDeletedOut);
                if (!status.isOK()) {
                    break;
                }
            }
        }
    }

    return status;
}

void IndexCatalogImpl::unindexRecord(OperationContext* opCtx,
                                     const CollectionPtr& collection,
                                     const BSONObj& obj,
                                     const RecordId& loc,
                                     bool noWarn,
                                     int64_t* keysDeletedOut,
                                     CheckRecordId checkRecordId) const {
    if (keysDeletedOut) {
        *keysDeletedOut = 0;
    }

    for (IndexCatalogEntryContainer::const_iterator it = _readyIndexes.begin();
         it != _readyIndexes.end();
         ++it) {
        const IndexCatalogEntry* entry = it->get();

        bool logIfError = !noWarn;
        _unindexRecord(
            opCtx, collection, entry, obj, loc, logIfError, keysDeletedOut, checkRecordId);
    }

    for (IndexCatalogEntryContainer::const_iterator it = _buildingIndexes.begin();
         it != _buildingIndexes.end();
         ++it) {
        const IndexCatalogEntry* entry = it->get();

        // If it's a background index, we DO NOT want to log anything.
        bool logIfError = entry->isReady() ? !noWarn : false;
        _unindexRecord(
            opCtx, collection, entry, obj, loc, logIfError, keysDeletedOut, checkRecordId);
    }
}

StatusWith<int64_t> IndexCatalogImpl::compactIndexes(OperationContext* opCtx,
                                                     const CompactOptions& options) const {

    int64_t estimatedBytes = 0;
    for (IndexCatalogEntryContainer::const_iterator it = _readyIndexes.begin();
         it != _readyIndexes.end();
         ++it) {
        const IndexCatalogEntry* entry = it->get();

        LOGV2_DEBUG(20363,
                    1,
                    "compacting index: {entry_descriptor}",
                    "entry_descriptor"_attr = *(entry->descriptor()));
        auto status = entry->accessMethod()->compact(
            opCtx, *shard_role_details::getRecoveryUnit(opCtx), options);
        if (!status.isOK()) {
            LOGV2_ERROR(20377,
                        "Failed to compact index",
                        "index"_attr = *(entry->descriptor()),
                        "error"_attr = redact(status.getStatus()));
            return status;
        }
        estimatedBytes += status.getValue();
    }
    return estimatedBytes;
}

std::string::size_type IndexCatalogImpl::getLongestIndexNameLength(OperationContext* opCtx) const {
    auto it = getIndexIterator(InclusionPolicy::kAll);
    std::string::size_type longestIndexNameLength = 0;
    while (it->more()) {
        auto thisLength = it->next()->descriptor()->indexName().length();
        if (thisLength > longestIndexNameLength)
            longestIndexNameLength = thisLength;
    }
    return longestIndexNameLength;
}

BSONObj IndexCatalogImpl::fixIndexKey(const BSONObj& key) const {
    if (IndexDescriptor::isIdIndexPattern(key)) {
        return _idObj;
    }
    if (key["_id"].type() == BSONType::boolean && key.nFields() == 1) {
        return _idObj;
    }
    return key;
}

void IndexCatalogImpl::prepareInsertDeleteOptions(OperationContext* opCtx,
                                                  const NamespaceString& ns,
                                                  const IndexDescriptor* desc,
                                                  InsertDeleteOptions* options) const {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->shouldRelaxIndexConstraints(opCtx, ns)) {
        options->getKeysMode = InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraints;
    } else {
        options->getKeysMode = InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints;
    }

    // Don't allow dups for Id key. Allow dups for non-unique keys or when constraints relaxed.
    if (desc->isIdIndex()) {
        options->dupsAllowed = false;
    } else {
        options->dupsAllowed = !desc->unique() ||
            options->getKeysMode ==
                InsertDeleteOptions::ConstraintEnforcementMode::kRelaxConstraints;
    }
}

void IndexCatalogImpl::indexBuildSuccess(OperationContext* opCtx,
                                         Collection* coll,
                                         IndexCatalogEntry* index) {
    // This function can be called inside of a WriteUnitOfWork, which can still encounter a write
    // conflict. We don't need to reset any in-memory state as a new writable collection is fetched
    // when retrying.
    auto releasedEntry = _buildingIndexes.release(index->descriptor());
    invariant(releasedEntry.get() == index);
    _indexUpdateIdentifier.reset();
    _readyIndexes.add(std::move(releasedEntry));

    index->setIndexBuildInterceptor(nullptr);
    index->setIsReady(true);
    _rebuildIndexUpdateIdentifier();
}

StatusWith<BSONObj> IndexCatalogImpl::_fixIndexSpec(OperationContext* opCtx,
                                                    const CollectionPtr& collection,
                                                    const BSONObj& spec) const {
    auto statusWithSpec = adjustIndexSpecObject(spec);
    if (!statusWithSpec.isOK()) {
        return statusWithSpec;
    }
    BSONObj o = statusWithSpec.getValue();

    BSONObjBuilder b;

    // We've already verified in IndexCatalog::_isSpecOk() that the index version is present and
    // that it is representable as a 32-bit integer.
    auto vElt = o["v"];
    invariant(vElt);

    b.append("v", vElt.numberInt());

    if (o["unique"].trueValue())
        b.appendBool("unique", true);  // normalize to bool true in case was int 1 or something...

    if (o["hidden"].trueValue())
        b.appendBool("hidden", true);  // normalize to bool true in case was int 1 or something...

    if (o["prepareUnique"].trueValue())
        b.appendBool("prepareUnique",
                     true);  // normalize to bool true in case was int 1 or something...

    BSONObj key = fixIndexKey(o["key"].Obj());
    b.append("key", key);

    StringData name = o["name"].checkAndGetStringData();
    if (IndexDescriptor::isIdIndexPattern(key)) {
        name = IndexConstants::kIdIndexName;
    }
    b.append("name", name);

    // During repair, if the 'ns' field exists in the index spec, do not remove it as repair can be
    // running on old data files from other mongod versions. Removing the 'ns' field during repair
    // would prevent the data files from starting up on the original mongod version as the 'ns'
    // field is required to be present in 3.6 and 4.0.
    if (storageGlobalParams.repair && o.hasField("ns")) {
        b.append("ns", o.getField("ns").String());
    }

    {
        BSONObjIterator i(o);
        while (i.more()) {
            BSONElement e = i.next();
            std::string s = e.fieldName();

            if (s == "_id") {
                // skip
            } else if (s == "dropDups" || s == "ns") {
                // dropDups is silently ignored and removed from the spec as of SERVER-14710.
                // ns is removed from the spec as of 4.4.
            } else if (s == "v" || s == "unique" || s == "key" || s == "name" || s == "hidden" ||
                       s == "prepareUnique") {
                // covered above
            } else {
                b.append(e);
            }
        }
    }

    return b.obj();
}

}  // namespace mongo
