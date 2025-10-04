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


#include "mongo/db/cloner.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/internal_auth.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/cloner_gen.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/ddl/list_collections_filter.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/local_catalog/ddl/list_indexes_gen.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/insert.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <map>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
static constexpr auto clonerAppName = "Cloner";
}  // namespace

using IndexVersion = IndexDescriptor::IndexVersion;

MONGO_FAIL_POINT_DEFINE(movePrimaryFailPoint);

BSONElement getErrField(const BSONObj& o);

BSONObj DefaultClonerImpl::_getIdIndexSpec(const std::list<BSONObj>& indexSpecs) {
    for (auto&& indexSpec : indexSpecs) {
        BSONElement indexName;
        uassertStatusOK(bsonExtractTypedField(
            indexSpec, IndexDescriptor::kIndexNameFieldName, BSONType::string, &indexName));
        if (indexName.valueStringData() == IndexConstants::kIdIndexName) {
            return indexSpec;
        }
    }
    return BSONObj();
}

struct DefaultClonerImpl::BatchHandler {
    BatchHandler(OperationContext* opCtx, const DatabaseName& dbName)
        : lastLog(0), opCtx(opCtx), _dbName(dbName), numSeen(0), saveLast(0) {}

    void operator()(DBClientCursor& cursor) {
        const auto acquireCollectionFn = [&](OperationContext* opCtx) -> CollectionAcquisition {
            return acquireCollection(
                opCtx,
                CollectionAcquisitionRequest(nss,
                                             PlacementConcern::kPretendUnsharded,
                                             repl::ReadConcernArgs::get(opCtx),
                                             AcquisitionPrerequisites::kWrite),
                MODE_X);
        };

        boost::optional<Lock::DBLock> dbLock;
        dbLock.emplace(opCtx, _dbName, MODE_IX);
        uassert(ErrorCodes::NotWritablePrimary,
                str::stream() << "Not primary while cloning collection "
                              << nss.toStringForErrorMsg(),
                !opCtx->writesAreReplicated() ||
                    repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

        boost::optional<CollectionAcquisition> collection;
        auto databaseHolder = DatabaseHolder::get(opCtx);
        Database* db;
        writeConflictRetry(opCtx, "createCollection", nss, [&] {
            opCtx->checkForInterrupt();

            db = databaseHolder->openDb(opCtx, _dbName);
            collection = acquireCollectionFn(opCtx);

            // Make sure database still exists after we resume from the temp release
            if (!collection->exists()) {
                OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                    unsafeCreateCollection(opCtx,
                                           nss,
                                           /* forceCSRAsUnknownAfterCollectionCreation */ true);
                WriteUnitOfWork wunit(opCtx);
                ScopedLocalCatalogWriteFence fence(opCtx, &collection.get());
                const bool createDefaultIndexes = true;
                CollectionOptions collectionOptions = uassertStatusOK(CollectionOptions::parse(
                    from_options, CollectionOptions::ParseKind::parseForCommand));
                invariant(db->userCreateNS(
                              opCtx, nss, collectionOptions, createDefaultIndexes, from_id_index),
                          str::stream() << "collection creation failed during clone ["
                                        << nss.toStringForErrorMsg() << "]");
                wunit.commit();
            }
        });
        invariant(collection->exists(),
                  str::stream() << "Missing collection during clone [" << nss.toStringForErrorMsg()
                                << "]");

        while (cursor.moreInCurrentBatch()) {
            if (numSeen % 128 == 127) {
                time_t now = time(nullptr);
                if (now - lastLog >= 60) {
                    // report progress
                    if (lastLog)
                        LOGV2(20412, "clone", logAttrs(nss), "numSeen"_attr = numSeen);
                    lastLog = now;
                }
                opCtx->checkForInterrupt();

                collection.reset();
                dbLock.reset();

                CurOp::get(opCtx)->yielded();

                dbLock.emplace(opCtx, _dbName, MODE_IX);

                // Check if everything is still all right.
                if (opCtx->writesAreReplicated()) {
                    uassert(
                        ErrorCodes::PrimarySteppedDown,
                        str::stream() << "Cannot write to ns: " << nss.toStringForErrorMsg()
                                      << " after yielding",
                        repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));
                }

                writeConflictRetry(opCtx, "reAcquireCollection", nss, [&] {
                    opCtx->checkForInterrupt();
                    db = databaseHolder->getDb(opCtx, _dbName);
                    uassert(28593,
                            str::stream() << "Database " << _dbName.toStringForErrorMsg()
                                          << " dropped while cloning",
                            db != nullptr);

                    collection.emplace(acquireCollectionFn(opCtx));
                    uassert(28594,
                            str::stream() << "Collection " << nss.toStringForErrorMsg()
                                          << " dropped while cloning",
                            collection->exists());
                });
            }

            BSONObj tmp = cursor.nextSafe();

            /* assure object is valid.  note this will slow us down a little. */
            // We allow cloning of collections containing decimal data even if decimal is
            // disabled.
            const Status status = validateBSON(tmp.objdata(), tmp.objsize());
            if (!status.isOK()) {
                if (gSkipCorruptDocumentsWhenCloning.load()) {
                    LOGV2_WARNING(20423,
                                  "Cloner: found corrupt document; skipping",
                                  logAttrs(nss),
                                  "error"_attr = redact(status));
                    continue;
                }
                str::stream ss;
                ss << "Cloner: found corrupt document in " << nss.toStringForErrorMsg() << ": "
                   << redact(status);
                msgasserted(28531, ss);
            }

            MONGO_verify(collection);
            ++numSeen;

            writeConflictRetry(opCtx, "cloner insert", nss, [&] {
                opCtx->checkForInterrupt();

                WriteUnitOfWork wunit(opCtx);

                BSONObj doc = tmp;
                Status status = collection_internal::insertDocument(opCtx,
                                                                    collection->getCollectionPtr(),
                                                                    InsertStatement(doc),
                                                                    nullptr /* OpDebug */,
                                                                    true);
                if (!status.isOK() && status.code() != ErrorCodes::DuplicateKey) {
                    LOGV2_ERROR(20424,
                                "Exception cloning document",
                                logAttrs(nss),
                                "error"_attr = redact(status),
                                "document"_attr = redact(doc));
                    uassertStatusOK(status);
                }
                if (status.isOK()) {
                    wunit.commit();
                }
            });

            static Rarely sampler;
            if (sampler.tick() && (time(nullptr) - saveLast > 60)) {
                LOGV2(20413,
                      "Number of objects cloned so far from collection",
                      "number"_attr = numSeen,
                      logAttrs(nss));
                saveLast = time(nullptr);
            }
        }
    }

    time_t lastLog;
    OperationContext* opCtx;
    const DatabaseName _dbName;

    int64_t numSeen;
    NamespaceString nss;
    BSONObj from_options;
    BSONObj from_id_index;
    time_t saveLast;
};

/**
 * Copy the specified collection.
 */
void DefaultClonerImpl::_copy(OperationContext* opCtx,
                              const DatabaseName& toDBName,
                              const NamespaceString& nss,
                              const BSONObj& from_opts,
                              const BSONObj& from_id_index) {
    LOGV2_DEBUG(20414,
                2,
                "\t\tcloning collection",
                logAttrs(nss),
                "conn_getServerAddress"_attr = getConn()->getServerAddress());

    BatchHandler batchHandler{opCtx, toDBName};
    batchHandler.numSeen = 0;
    batchHandler.nss = nss;
    batchHandler.from_options = from_opts;
    batchHandler.from_id_index = from_id_index;
    batchHandler.saveLast = time(nullptr);

    FindCommandRequest findCmd{nss};
    findCmd.setNoCursorTimeout(true);
    findCmd.setReadConcern(repl::ReadConcernArgs::kLocal);
    if (shouldUseRawDataOperations(VersionContext::getDecoration(opCtx))) {
        findCmd.setRawData(true);
    }
    auto cursor = getConn()->find(
        std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryOnly}, ExhaustMode::kOn);

    // Process the results of the cursor in batches.
    while (cursor->more()) {
        batchHandler(*cursor);
    }
}

void DefaultClonerImpl::_copyIndexes(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const BSONObj& from_opts,
                                     const std::list<BSONObj>& from_indexes) {
    LOGV2_DEBUG(20415,
                2,
                "\t\t copyIndexes",
                logAttrs(nss),
                "conn_getServerAddress"_attr = getConn()->getServerAddress());

    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Not primary while copying indexes from " << nss.toStringForErrorMsg()
                          << " (Cloner)",
            !opCtx->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

    if (from_indexes.empty())
        return;

    writeConflictRetry(opCtx, "_copyIndexes", nss, [&] {
        opCtx->checkForInterrupt();
        WriteUnitOfWork wunit(opCtx);

        auto collLock =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest(nss,
                                                           PlacementConcern::kPretendUnsharded,
                                                           repl::ReadConcernArgs::get(opCtx),
                                                           AcquisitionPrerequisites::kWrite),
                              MODE_X);

        CollectionWriter collection(opCtx, &collLock);
        invariant(collection,
                  str::stream() << "Missing collection " << nss.toStringForErrorMsg()
                                << " (Cloner)");
        uassert(10659000,
                str::stream() << "Collection " << nss.toStringForErrorMsg()
                              << " must be empty before copying indexes",
                collLock.getCollectionPtr()->isEmpty(opCtx));
        auto indexCatalog = collection->getIndexCatalog();
        auto indexesToBuild = indexCatalog->removeExistingIndexesNoChecks(
            opCtx, collection.get(), {std::begin(from_indexes), std::end(from_indexes)});
        if (indexesToBuild.empty()) {
            return;
        }

        auto fromMigrate = false;
        IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
            opCtx, collection, indexesToBuild, fromMigrate);
        wunit.commit();
    });
}

StatusWith<std::vector<BSONObj>> DefaultClonerImpl::_filterCollectionsForClone(
    const DatabaseName& fromDBName, const std::list<BSONObj>& initialCollections) {
    std::vector<BSONObj> finalCollections;
    for (auto&& collection : initialCollections) {
        LOGV2_DEBUG(20418, 2, "\t cloner got {collection}", "collection"_attr = collection);

        BSONElement collectionOptions = collection["options"];
        if (collectionOptions.isABSONObj()) {
            auto statusWithCollectionOptions = CollectionOptions::parse(
                collectionOptions.Obj(), CollectionOptions::ParseKind::parseForCommand);
            if (!statusWithCollectionOptions.isOK()) {
                return statusWithCollectionOptions.getStatus();
            }
        }

        std::string collectionName;
        auto status = bsonExtractStringField(collection, "name", &collectionName);
        if (!status.isOK()) {
            return status;
        }

        const auto nss = NamespaceStringUtil::deserialize(fromDBName, collectionName.c_str());
        if (nss.isSystem()) {
            if (!nss.isLegalClientSystemNS()) {
                LOGV2_DEBUG(20419, 2, "\t\t not cloning because system collection");
                continue;
            }
        }

        finalCollections.push_back(collection.getOwned());
    }
    return finalCollections;
}

Status DefaultClonerImpl::_createCollectionsForDb(
    OperationContext* opCtx,
    const std::vector<CreateCollectionParams>& createCollectionParams,
    const DatabaseName& dbName) {
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->openDb(opCtx, dbName);
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IX));

    auto collCount = 0;
    for (auto&& params : createCollectionParams) {
        if (MONGO_unlikely(movePrimaryFailPoint.shouldFail()) && collCount > 0) {
            return Status(ErrorCodes::CommandFailed, "movePrimary failed due to failpoint");
        }
        collCount++;

        const auto nss = NamespaceStringUtil::deserialize(dbName, params.collectionName);

        Status status = writeConflictRetry(opCtx, "createCollection", nss, [&] {
            opCtx->checkForInterrupt();
            WriteUnitOfWork wunit(opCtx);
            BSONObjBuilder optionsBuilder;
            optionsBuilder.appendElements(params.collectionInfo["options"].Obj());

            auto collection =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(nss,
                                                               PlacementConcern::kPretendUnsharded,
                                                               repl::ReadConcernArgs::get(opCtx),
                                                               AcquisitionPrerequisites::kWrite),
                                  MODE_IX);
            if (collection.exists()) {
                if (!params.trackedColls) {
                    // If the collection is unsharded then we want to fail when a collection
                    // we're trying to create already exists.
                    return Status(ErrorCodes::NamespaceExists,
                                  str::stream() << "unsharded collection with same namespace "
                                                << nss.toStringForErrorMsg() << " already exists.");
                }

                // If the collection is sharded and a collection with the same name already
                // exists on the target, we check if the existing collection's UUID matches that
                // of the one we're trying to create. If it does, we treat the create as a
                // no-op; if it doesn't match, we return an error.
                const auto& existingOpts = collection.getCollectionPtr()->getCollectionOptions();
                const UUID clonedUUID =
                    uassertStatusOK(UUID::parse(params.collectionInfo["info"]["uuid"]));
                if (clonedUUID != existingOpts.uuid) {
                    return Status(ErrorCodes::InvalidOptions,
                                  str::stream()
                                      << "sharded collection with same namespace "
                                      << nss.toStringForErrorMsg()
                                      << " already exists, but UUIDs don't match. Existing UUID is "
                                      << existingOpts.uuid << " and new UUID is " << clonedUUID);
                }
            } else {
                uassertStatusOK(userAllowedCreateNS(opCtx, nss));

                // If the collection does not already exist and is tracked, we create a new
                // collection on the target shard with the UUID of the original collection and
                // copy the options and secondary indexes. If the collection does not already
                // exist and is untracked, we create a new collection with its own UUID and copy
                // the options and secondary indexes of the original collection.
                if (params.trackedColls) {
                    optionsBuilder.append(params.collectionInfo["info"]["uuid"]);
                }

                const bool createDefaultIndexes = true;
                auto options = optionsBuilder.obj();

                CollectionOptions collectionOptions = uassertStatusOK(CollectionOptions::parse(
                    options, CollectionOptions::ParseKind::parseForStorage));

                {
                    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                        unsafeCreateCollection(opCtx,
                                               nss,
                                               /* forceCSRAsUnknownAfterCollectionCreation */ true);
                    Status createStatus = db->userCreateNS(
                        opCtx, nss, collectionOptions, createDefaultIndexes, params.idIndexSpec);
                    if (!createStatus.isOK()) {
                        return createStatus;
                    }
                }
            }

            wunit.commit();
            return Status::OK();
        });

        // Break early if one of the creations fails.
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status DefaultClonerImpl::setupConn(OperationContext* opCtx, const std::string& masterHost) {
    invariant(!_conn);
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    auto statusWithMasterHost = ConnectionString::parse(masterHost);

    if (!statusWithMasterHost.isOK()) {
        return statusWithMasterHost.getStatus();
    }

    const ConnectionString cs(statusWithMasterHost.getValue());

    bool masterSameProcess = false;
    std::vector<HostAndPort> csServers = cs.getServers();
    for (std::vector<HostAndPort>::const_iterator iter = csServers.begin(); iter != csServers.end();
         ++iter) {
        if (!repl::isSelf(*iter, opCtx->getServiceContext()))
            continue;

        masterSameProcess = true;
        break;
    }

    if (masterSameProcess) {
        // Guard against re-entrance
        return Status(ErrorCodes::IllegalOperation, "can't clone from self (localhost)");
    }

    // Set up connection.
    auto swConn = cs.connect(clonerAppName);
    if (!swConn.isOK()) {
        return swConn.getStatus();
    }

    _conn = std::move(swConn.getValue());

    if (auth::isInternalAuthSet()) {
        try {
            getConn()->authenticateInternalUser();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }
    return Status::OK();
}

StatusWith<std::vector<BSONObj>> DefaultClonerImpl::getListOfCollections(
    OperationContext* opCtx, const DatabaseName& dbName, const std::string& masterHost) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    std::vector<BSONObj> collsToClone;
    if (!_conn) {
        auto connStatus = setupConn(opCtx, masterHost);
        if (!connStatus.isOK()) {
            return connStatus;
        }
    }
    // Gather the list of collections to clone
    // No tenant id required as the db cloner is only used for moving primary dbs in sharding.
    ListCollections listCollectionsCmd;
    listCollectionsCmd.setDbName(dbName);
    listCollectionsCmd.setFilter(ListCollectionsFilter::makeTypeCollectionFilter());
    if (shouldUseRawDataOperations(VersionContext::getDecoration(opCtx))) {
        listCollectionsCmd.setRawData(true);
    }
    auto initialCollections = uassertStatusOK(getConn()->runExhaustiveCursorCommand(
        dbName, listCollectionsCmd.toBSON(), QueryOption_SecondaryOk));
    return _filterCollectionsForClone(dbName, initialCollections);
}

Status DefaultClonerImpl::copyDb(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const std::string& masterHost,
                                 const std::vector<NamespaceString>& trackedColls,
                                 std::set<std::string>* clonedColls) {
    invariant(clonedColls && clonedColls->empty(),
              str::stream() << masterHost << ":" << dbName.toStringForErrorMsg());
    // This function can potentially block for a long time on network activity, so holding of
    // locks is disallowed.
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    auto toCloneStatus = getListOfCollections(opCtx, dbName, masterHost);
    if (!toCloneStatus.isOK()) {
        return toCloneStatus.getStatus();
    }

    auto toClone = toCloneStatus.getValue();

    std::vector<CreateCollectionParams> createCollectionParams;
    for (auto&& collection : toClone) {
        CreateCollectionParams params;
        params.collectionName = collection["name"].String();
        params.collectionInfo = collection;
        if (auto idIndex = collection["idIndex"]) {
            params.idIndexSpec = idIndex.Obj();
        }

        const auto nss = NamespaceStringUtil::deserialize(dbName, params.collectionName);
        if (std::find(trackedColls.begin(), trackedColls.end(), nss) != trackedColls.end()) {
            params.trackedColls = true;
        }
        createCollectionParams.push_back(params);
    }

    // Get index specs for each collection.
    std::map<StringData, std::list<BSONObj>> collectionIndexSpecs;
    for (auto&& params : createCollectionParams) {
        const auto nss = NamespaceStringUtil::deserialize(dbName, params.collectionName);
        ListIndexes listIndexesCmd(nss);
        if (shouldUseRawDataOperations(VersionContext::getDecoration(opCtx))) {
            listIndexesCmd.setRawData(true);
        }
        auto indexSpecs = uassertStatusOK(
            getConn()->runExhaustiveCursorCommand(nss.dbName(), listIndexesCmd.toBSON()));

        collectionIndexSpecs[params.collectionName] = indexSpecs;

        if (params.idIndexSpec.isEmpty()) {
            params.idIndexSpec = _getIdIndexSpec(indexSpecs);
        }
    }

    {
        Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
        uassert(ErrorCodes::NotWritablePrimary,
                str::stream() << "Not primary while cloning database "
                              << dbName.toStringForErrorMsg()
                              << " (after getting list of collections to clone)",
                !opCtx->writesAreReplicated() ||
                    repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                                         dbName));

        auto status = _createCollectionsForDb(opCtx, createCollectionParams, dbName);
        if (!status.isOK()) {
            return status;
        }

        // now build the secondary indexes
        for (auto&& params : createCollectionParams) {

            // Indexes of sharded collections are not copied: the primary shard is not required
            // to have all indexes. The listIndexes cmd is sent to the shard owning the MinKey
            // value.
            if (params.trackedColls) {
                continue;
            }

            LOGV2(20422, "Copying indexes", "collectionInfo"_attr = params.collectionInfo);

            const auto nss = NamespaceStringUtil::deserialize(dbName, params.collectionName);

            _copyIndexes(opCtx,
                         nss,
                         params.collectionInfo["options"].Obj(),
                         collectionIndexSpecs[params.collectionName]);
        }
    }

    for (auto&& params : createCollectionParams) {
        if (params.trackedColls) {
            continue;
        }

        LOGV2_DEBUG(20420,
                    2,
                    "  really will clone: {params_collectionInfo}",
                    "params_collectionInfo"_attr = params.collectionInfo);

        const auto nss = NamespaceStringUtil::deserialize(dbName, params.collectionName);

        clonedColls->insert(
            NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));

        LOGV2_DEBUG(20421, 1, "\t\t cloning", logAttrs(nss), "host"_attr = masterHost);

        _copy(opCtx, dbName, nss, params.collectionInfo["options"].Obj(), params.idIndexSpec);
    }

    return Status::OK();
}

bool DefaultClonerImpl::shouldUseRawDataOperations(const VersionContext& vCtx) {
    return gFeatureFlagAllBinariesSupportRawDataOperations.isEnabled(
        vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

Cloner::Cloner() : Cloner(std::make_unique<DefaultClonerImpl>()) {}

Status Cloner::copyDb(OperationContext* opCtx,
                      const DatabaseName& dbName,
                      const std::string& masterHost,
                      const std::vector<NamespaceString>& trackedColls,
                      std::set<std::string>* clonedColls) {
    return _clonerImpl->copyDb(opCtx, dbName, masterHost, trackedColls, clonedColls);
}

StatusWith<std::vector<BSONObj>> Cloner::getListOfCollections(OperationContext* opCtx,
                                                              const DatabaseName& dbName,
                                                              const std::string& masterHost) {
    return _clonerImpl->getListOfCollections(opCtx, dbName, masterHost);
}

}  // namespace mongo
