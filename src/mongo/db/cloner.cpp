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


#include "mongo/platform/basic.h"

#include "mongo/db/cloner.h"

#include <algorithm>

#include "mongo/base/status.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/authenticate.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/cloner_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

using IndexVersion = IndexDescriptor::IndexVersion;

MONGO_FAIL_POINT_DEFINE(movePrimaryFailPoint);

BSONElement getErrField(const BSONObj& o);

BSONObj Cloner::_getIdIndexSpec(const std::list<BSONObj>& indexSpecs) {
    for (auto&& indexSpec : indexSpecs) {
        BSONElement indexName;
        uassertStatusOK(bsonExtractTypedField(
            indexSpec, IndexDescriptor::kIndexNameFieldName, String, &indexName));
        if (indexName.valueStringData() == "_id_"_sd) {
            return indexSpec;
        }
    }
    return BSONObj();
}

Cloner::Cloner() {}

struct Cloner::BatchHandler {
    BatchHandler(OperationContext* opCtx, const std::string& dbName)
        : lastLog(0), opCtx(opCtx), _dbName(dbName), numSeen(0), saveLast(0) {}

    void operator()(DBClientCursor& cursor) {
        boost::optional<Lock::DBLock> dbLock;
        // TODO SERVER-63111 Once the Cloner holds a DatabaseName obj, use _dbName directly
        DatabaseName dbName(boost::none, _dbName);
        dbLock.emplace(opCtx, dbName, MODE_X);
        uassert(ErrorCodes::NotWritablePrimary,
                str::stream() << "Not primary while cloning collection " << nss,
                !opCtx->writesAreReplicated() ||
                    repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

        // Make sure database still exists after we resume from the temp release
        auto databaseHolder = DatabaseHolder::get(opCtx);
        auto db = databaseHolder->openDb(opCtx, dbName);
        auto catalog = CollectionCatalog::get(opCtx);
        auto collection = catalog->lookupCollectionByNamespace(opCtx, nss);
        if (!collection) {
            writeConflictRetry(opCtx, "createCollection", nss.ns(), [&] {
                opCtx->checkForInterrupt();

                WriteUnitOfWork wunit(opCtx);
                const bool createDefaultIndexes = true;
                CollectionOptions collectionOptions = uassertStatusOK(CollectionOptions::parse(
                    from_options, CollectionOptions::ParseKind::parseForCommand));
                invariant(db->userCreateNS(
                              opCtx, nss, collectionOptions, createDefaultIndexes, from_id_index),
                          str::stream()
                              << "collection creation failed during clone [" << nss << "]");
                wunit.commit();
                collection = catalog->lookupCollectionByNamespace(opCtx, nss);
                invariant(collection,
                          str::stream() << "Missing collection during clone [" << nss << "]");
            });
        }

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

                dbLock.reset();

                CurOp::get(opCtx)->yielded();

                // TODO SERVER-63111 Once the cloner takes in a DatabaseName obj, use _dbName
                // directly
                DatabaseName dbName(boost::none, _dbName);
                dbLock.emplace(opCtx, dbName, MODE_X);

                // Check if everything is still all right.
                if (opCtx->writesAreReplicated()) {
                    uassert(
                        ErrorCodes::PrimarySteppedDown,
                        str::stream() << "Cannot write to ns: " << nss << " after yielding",
                        repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));
                }

                db = databaseHolder->getDb(opCtx, dbName);
                uassert(28593,
                        str::stream() << "Database " << _dbName << " dropped while cloning",
                        db != nullptr);

                collection = catalog->lookupCollectionByNamespace(opCtx, nss);
                uassert(28594,
                        str::stream() << "Collection " << nss << " dropped while cloning",
                        collection);
            }

            BSONObj tmp = cursor.nextSafe();

            /* assure object is valid.  note this will slow us down a little. */
            // We allow cloning of collections containing decimal data even if decimal is disabled.
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
                ss << "Cloner: found corrupt document in " << nss << ": " << redact(status);
                msgasserted(28531, ss);
            }

            verify(collection);
            ++numSeen;

            writeConflictRetry(opCtx, "cloner insert", nss.ns(), [&] {
                opCtx->checkForInterrupt();

                WriteUnitOfWork wunit(opCtx);

                BSONObj doc = tmp;
                OpDebug* const nullOpDebug = nullptr;
                Status status =
                    collection->insertDocument(opCtx, InsertStatement(doc), nullOpDebug, true);
                if (!status.isOK() && status.code() != ErrorCodes::DuplicateKey) {
                    LOGV2_ERROR(20424,
                                "error: exception cloning object",
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
    const std::string _dbName;

    int64_t numSeen;
    NamespaceString nss;
    BSONObj from_options;
    BSONObj from_id_index;
    time_t saveLast;
};

/**
 * Copy the specified collection.
 */
void Cloner::_copy(OperationContext* opCtx,
                   const std::string& toDBName,
                   const NamespaceString& nss,
                   const BSONObj& from_opts,
                   const BSONObj& from_id_index,
                   DBClientBase* conn) {
    LOGV2_DEBUG(20414,
                2,
                "\t\tcloning collection",
                logAttrs(nss),
                "conn_getServerAddress"_attr = conn->getServerAddress());

    BatchHandler batchHandler{opCtx, toDBName};
    batchHandler.numSeen = 0;
    batchHandler.nss = nss;
    batchHandler.from_options = from_opts;
    batchHandler.from_id_index = from_id_index;
    batchHandler.saveLast = time(nullptr);

    FindCommandRequest findCmd{nss};
    findCmd.setNoCursorTimeout(true);
    findCmd.setReadConcern(repl::ReadConcernArgs::kLocal);
    auto cursor = conn->find(
        std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryOnly}, ExhaustMode::kOn);

    // Process the results of the cursor in batches.
    while (cursor->more()) {
        batchHandler(*cursor);
    }
}

void Cloner::_copyIndexes(OperationContext* opCtx,
                          const std::string& toDBName,
                          const NamespaceString& nss,
                          const BSONObj& from_opts,
                          const std::list<BSONObj>& from_indexes,
                          DBClientBase* conn) {
    LOGV2_DEBUG(20415,
                2,
                "\t\t copyIndexes",
                logAttrs(nss),
                "conn_getServerAddress"_attr = conn->getServerAddress());

    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Not primary while copying indexes from " << nss << " (Cloner)",
            !opCtx->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

    if (from_indexes.empty())
        return;

    CollectionWriter collection(opCtx, nss);
    invariant(collection, str::stream() << "Missing collection " << nss << " (Cloner)");

    auto indexCatalog = collection->getIndexCatalog();
    auto indexesToBuild = indexCatalog->removeExistingIndexesNoChecks(
        opCtx, collection.get(), {std::begin(from_indexes), std::end(from_indexes)});
    if (indexesToBuild.empty()) {
        return;
    }

    auto fromMigrate = false;
    writeConflictRetry(opCtx, "_copyIndexes", nss.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);
        IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
            opCtx, collection, indexesToBuild, fromMigrate);
        wunit.commit();
    });
}

StatusWith<std::vector<BSONObj>> Cloner::_filterCollectionsForClone(
    const std::string& fromDBName, const std::list<BSONObj>& initialCollections) {
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

        const NamespaceString ns(fromDBName, collectionName.c_str());
        if (ns.isSystem()) {
            if (!ns.isLegalClientSystemNS(serverGlobalParams.featureCompatibility)) {
                LOGV2_DEBUG(20419, 2, "\t\t not cloning because system collection");
                continue;
            }
        }

        finalCollections.push_back(collection.getOwned());
    }
    return finalCollections;
}

Status Cloner::_createCollectionsForDb(
    OperationContext* opCtx,
    const std::vector<CreateCollectionParams>& createCollectionParams,
    const std::string& dbName) {
    auto databaseHolder = DatabaseHolder::get(opCtx);
    const DatabaseName tenantDbName(boost::none, dbName);
    auto db = databaseHolder->openDb(opCtx, tenantDbName);
    invariant(opCtx->lockState()->isDbLockedForMode(tenantDbName, MODE_X));

    auto catalog = CollectionCatalog::get(opCtx);
    auto collCount = 0;
    for (auto&& params : createCollectionParams) {
        if (MONGO_unlikely(movePrimaryFailPoint.shouldFail()) && collCount > 0) {
            return Status(ErrorCodes::CommandFailed, "movePrimary failed due to failpoint");
        }
        collCount++;

        BSONObjBuilder optionsBuilder;
        optionsBuilder.appendElements(params.collectionInfo["options"].Obj());

        const NamespaceString nss(dbName, params.collectionName);

        uassertStatusOK(userAllowedCreateNS(opCtx, nss));
        Status status = writeConflictRetry(opCtx, "createCollection", nss.ns(), [&] {
            opCtx->checkForInterrupt();
            WriteUnitOfWork wunit(opCtx);

            CollectionPtr collection = catalog->lookupCollectionByNamespace(opCtx, nss);
            if (collection) {
                if (!params.shardedColl) {
                    // If the collection is unsharded then we want to fail when a collection
                    // we're trying to create already exists.
                    return Status(ErrorCodes::NamespaceExists,
                                  str::stream() << "unsharded collection with same namespace "
                                                << nss.ns() << " already exists.");
                }

                // If the collection is sharded and a collection with the same name already
                // exists on the target, we check if the existing collection's UUID matches
                // that of the one we're trying to create. If it does, we treat the create
                // as a no-op; if it doesn't match, we return an error.
                const auto& existingOpts = collection->getCollectionOptions();
                const UUID clonedUUID =
                    uassertStatusOK(UUID::parse(params.collectionInfo["info"]["uuid"]));

                if (clonedUUID == existingOpts.uuid)
                    return Status::OK();

                return Status(ErrorCodes::InvalidOptions,
                              str::stream()
                                  << "sharded collection with same namespace " << nss.ns()
                                  << " already exists, but UUIDs don't match. Existing UUID is "
                                  << existingOpts.uuid << " and new UUID is " << clonedUUID);
            }

            // If the collection does not already exist and is sharded, we create a new
            // collection on the target shard with the UUID of the original collection and
            // copy the options and secondary indexes. If the collection does not already
            // exist and is unsharded, we create a new collection with its own UUID and
            // copy the options and secondary indexes of the original collection.

            if (params.shardedColl) {
                optionsBuilder.append(params.collectionInfo["info"]["uuid"]);
            }

            const bool createDefaultIndexes = true;
            auto options = optionsBuilder.obj();

            CollectionOptions collectionOptions = uassertStatusOK(
                CollectionOptions::parse(options, CollectionOptions::ParseKind::parseForStorage));

            {
                OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                    unsafeCreateCollection(opCtx);
                Status createStatus = db->userCreateNS(
                    opCtx, nss, collectionOptions, createDefaultIndexes, params.idIndexSpec);
                if (!createStatus.isOK()) {
                    return createStatus;
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

Status Cloner::copyDb(OperationContext* opCtx,
                      const std::string& dBName,
                      const std::string& masterHost,
                      const std::vector<NamespaceString>& shardedColls,
                      std::set<std::string>* clonedColls) {
    invariant(clonedColls && clonedColls->empty(), str::stream() << masterHost << ":" << dBName);
    // This function can potentially block for a long time on network activity, so holding of locks
    // is disallowed.
    invariant(!opCtx->lockState()->isLocked());

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
    auto swConn = cs.connect(StringData());
    if (!swConn.isOK()) {
        return swConn.getStatus();
    }
    auto& conn = swConn.getValue();

    if (auth::isInternalAuthSet()) {
        auto authStatus = conn->authenticateInternalUser();
        if (!authStatus.isOK()) {
            return authStatus;
        }
    }

    // Gather the list of collections to clone
    std::list<BSONObj> initialCollections =
        conn->getCollectionInfos(dBName, ListCollectionsFilter::makeTypeCollectionFilter());

    auto statusWithCollections = _filterCollectionsForClone(dBName, initialCollections);
    if (!statusWithCollections.isOK()) {
        return statusWithCollections.getStatus();
    }
    auto toClone = statusWithCollections.getValue();

    std::vector<CreateCollectionParams> createCollectionParams;
    for (auto&& collection : toClone) {
        CreateCollectionParams params;
        params.collectionName = collection["name"].String();
        params.collectionInfo = collection;
        if (auto idIndex = collection["idIndex"]) {
            params.idIndexSpec = idIndex.Obj();
        }

        const NamespaceString ns(dBName, params.collectionName);
        if (std::find(shardedColls.begin(), shardedColls.end(), ns) != shardedColls.end()) {
            params.shardedColl = true;
        }
        createCollectionParams.push_back(params);
    }

    // Get index specs for each collection.
    std::map<StringData, std::list<BSONObj>> collectionIndexSpecs;
    for (auto&& params : createCollectionParams) {
        const NamespaceString nss(dBName, params.collectionName);
        const bool includeBuildUUIDs = false;
        const int options = 0;
        auto indexSpecs = conn->getIndexSpecs(nss, includeBuildUUIDs, options);

        collectionIndexSpecs[params.collectionName] = indexSpecs;

        if (params.idIndexSpec.isEmpty()) {
            params.idIndexSpec = _getIdIndexSpec(indexSpecs);
        }
    }

    {
        // TODO SERVER-63111 Once the cloner takes in a DatabaseName obj, use dBName directly
        DatabaseName dbName(boost::none, dBName);
        Lock::DBLock dbXLock(opCtx, dbName, MODE_X);
        uassert(ErrorCodes::NotWritablePrimary,
                str::stream() << "Not primary while cloning database " << dBName
                              << " (after getting list of collections to clone)",
                !opCtx->writesAreReplicated() ||
                    repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                                         dBName));

        auto status = _createCollectionsForDb(opCtx, createCollectionParams, dBName);
        if (!status.isOK()) {
            return status;
        }

        // now build the secondary indexes
        for (auto&& params : createCollectionParams) {
            LOGV2(20422,
                  "copying indexes for: {collectionInfo}",
                  "Copying indexes",
                  "collectionInfo"_attr = params.collectionInfo);

            const NamespaceString nss(dBName, params.collectionName);


            _copyIndexes(opCtx,
                         dBName,
                         nss,
                         params.collectionInfo["options"].Obj(),
                         collectionIndexSpecs[params.collectionName],
                         conn.get());
        }
    }

    for (auto&& params : createCollectionParams) {
        if (params.shardedColl) {
            continue;
        }

        LOGV2_DEBUG(20420,
                    2,
                    "  really will clone: {params_collectionInfo}",
                    "params_collectionInfo"_attr = params.collectionInfo);

        const NamespaceString nss(dBName, params.collectionName);

        clonedColls->insert(nss.ns());

        LOGV2_DEBUG(20421, 1, "\t\t cloning", logAttrs(nss), "host"_attr = masterHost);

        _copy(opCtx,
              dBName,
              nss,
              params.collectionInfo["options"].Obj(),
              params.idIndexSpec,
              conn.get());
    }

    return Status::OK();
}

}  // namespace mongo
