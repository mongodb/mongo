/**
 *    Copyright (C) 2013-2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;

namespace {

const StringData kIndexesFieldName = "indexes"_sd;

/**
 * Parses the index specifications from 'cmdObj', validates them, and returns equivalent index
 * specifications that have any missing attributes filled in. If any index specification is
 * malformed, then an error status is returned.
 */
StatusWith<std::vector<BSONObj>> parseAndValidateIndexSpecs(
    const NamespaceString& ns,
    const BSONObj& cmdObj,
    ServerGlobalParams::FeatureCompatibilityVersions featureCompatibilityVersion) {
    bool hasIndexesField = false;

    std::vector<BSONObj> indexSpecs;
    for (auto&& cmdElem : cmdObj) {
        auto cmdElemFieldName = cmdElem.fieldNameStringData();

        if (kIndexesFieldName == cmdElemFieldName) {
            if (cmdElem.type() != BSONType::Array) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "The field '" << kIndexesFieldName
                                      << "' must be an array, but got "
                                      << typeName(cmdElem.type())};
            }

            for (auto&& indexesElem : cmdElem.Obj()) {
                if (indexesElem.type() != BSONType::Object) {
                    return {ErrorCodes::TypeMismatch,
                            str::stream() << "The elements of the '" << kIndexesFieldName
                                          << "' array must be objects, but got "
                                          << typeName(indexesElem.type())};
                }

                auto indexSpec =
                    validateIndexSpec(indexesElem.Obj(), ns, featureCompatibilityVersion);
                if (!indexSpec.isOK()) {
                    return indexSpec.getStatus();
                }
                indexSpecs.push_back(std::move(indexSpec.getValue()));
            }

            hasIndexesField = true;
        } else {
            // TODO SERVER-769: Validate top-level options to the "createIndexes" command.
            continue;
        }
    }

    if (!hasIndexesField) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "The '" << kIndexesFieldName
                              << "' field is a required argument of the createIndexes command"};
    }

    if (indexSpecs.empty()) {
        return {ErrorCodes::BadValue, "Must specify at least one index to create"};
    }

    return indexSpecs;
}

}  // namespace

/**
 * { createIndexes : "bar", indexes : [ { ns : "test.bar", key : { x : 1 }, name: "x_1" } ] }
 */
class CmdCreateIndex : public Command {
public:
    CmdCreateIndex() : Command("createIndexes") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual bool slaveOk() const {
        return false;
    }  // TODO: this could be made true...

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        ActionSet actions;
        actions.addAction(ActionType::createIndex);
        Privilege p(parseResourcePattern(dbname, cmdObj), actions);
        if (AuthorizationSession::get(client)->isAuthorizedForPrivilege(p))
            return Status::OK();
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result) {
        const NamespaceString ns(parseNs(dbname, cmdObj));

        Status status = userAllowedWriteNS(ns);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        const auto featureCompatibilityVersion =
            serverGlobalParams.featureCompatibilityVersion.load();
        auto specsWithStatus = parseAndValidateIndexSpecs(ns, cmdObj, featureCompatibilityVersion);
        if (!specsWithStatus.isOK()) {
            return appendCommandStatus(result, specsWithStatus.getStatus());
        }
        auto specs = std::move(specsWithStatus.getValue());

        // now we know we have to create index(es)
        // Note: createIndexes command does not currently respect shard versioning.
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbLock(txn->lockState(), ns.db(), MODE_X);
        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(ns)) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::NotMaster,
                       str::stream() << "Not primary while creating indexes in " << ns.ns()));
        }

        Database* db = dbHolder().get(txn, ns.db());
        if (!db) {
            db = dbHolder().openDb(txn, ns.db());
        }

        Collection* collection = db->getCollection(ns.ns());
        if (collection) {
            result.appendBool("createdCollectionAutomatically", false);
        } else {
            if (db->getViewCatalog()->lookup(txn, ns.ns())) {
                errmsg = "Cannot create indexes on a view";
                return appendCommandStatus(result, {ErrorCodes::CommandNotSupportedOnView, errmsg});
            }

            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                WriteUnitOfWork wunit(txn);
                collection = db->createCollection(txn, ns.ns(), CollectionOptions());
                invariant(collection);
                wunit.commit();
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createIndexes", ns.ns());
            result.appendBool("createdCollectionAutomatically", true);
        }

        const int numIndexesBefore = collection->getIndexCatalog()->numIndexesTotal(txn);
        result.append("numIndexesBefore", numIndexesBefore);

        auto client = txn->getClient();
        ScopeGuard lastOpSetterGuard =
            MakeObjGuard(repl::ReplClientInfo::forClient(client),
                         &repl::ReplClientInfo::setLastOpToSystemLastOpTime,
                         txn);

        MultiIndexBlock indexer(txn, collection);
        indexer.allowBackgroundBuilding();
        indexer.allowInterruption();

        const size_t origSpecsSize = specs.size();
        indexer.removeExistingIndexes(&specs);

        if (specs.size() == 0) {
            result.append("numIndexesAfter", numIndexesBefore);
            result.append("note", "all indexes already exist");
            return true;
        }

        if (specs.size() != origSpecsSize) {
            result.append("note", "index already exists");
        }

        for (size_t i = 0; i < specs.size(); i++) {
            const BSONObj& spec = specs[i];
            if (spec["unique"].trueValue()) {
                status = checkUniqueIndexConstraints(txn, ns.ns(), spec["key"].Obj());

                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }
            }
        }

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            uassertStatusOK(indexer.init(specs));
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createIndexes", ns.ns());

        // If we're a background index, replace exclusive db lock with an intent lock, so that
        // other readers and writers can proceed during this phase.
        if (indexer.getBuildInBackground()) {
            txn->recoveryUnit()->abandonSnapshot();
            dbLock.relockWithMode(MODE_IX);
            if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(ns)) {
                return appendCommandStatus(
                    result,
                    Status(ErrorCodes::NotMaster,
                           str::stream() << "Not primary while creating background indexes in "
                                         << ns.ns()));
            }
        }

        try {
            Lock::CollectionLock colLock(txn->lockState(), ns.ns(), MODE_IX);
            uassertStatusOK(indexer.insertAllDocumentsInCollection());
        } catch (const DBException& e) {
            invariant(e.getCode() != ErrorCodes::WriteConflict);
            // Must have exclusive DB lock before we clean up the index build via the
            // destructor of 'indexer'.
            if (indexer.getBuildInBackground()) {
                try {
                    // This function cannot throw today, but we will preemptively prepare for
                    // that day, to avoid data corruption due to lack of index cleanup.
                    txn->recoveryUnit()->abandonSnapshot();
                    dbLock.relockWithMode(MODE_X);
                    if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(ns)) {
                        return appendCommandStatus(
                            result,
                            Status(ErrorCodes::NotMaster,
                                   str::stream()
                                       << "Not primary while creating background indexes in "
                                       << ns.ns()
                                       << ": cleaning up index build failure due to "
                                       << e.toString()));
                    }
                } catch (...) {
                    std::terminate();
                }
            }
            throw;
        }
        // Need to return db lock back to exclusive, to complete the index build.
        if (indexer.getBuildInBackground()) {
            txn->recoveryUnit()->abandonSnapshot();
            dbLock.relockWithMode(MODE_X);
            uassert(ErrorCodes::NotMaster,
                    str::stream() << "Not primary while completing index build in " << dbname,
                    repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(ns));

            Database* db = dbHolder().get(txn, ns.db());
            uassert(28551, "database dropped during index build", db);
            uassert(28552, "collection dropped during index build", db->getCollection(ns.ns()));
        }

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wunit(txn);

            indexer.commit();

            for (size_t i = 0; i < specs.size(); i++) {
                std::string systemIndexes = ns.getSystemIndexesCollection();
                auto opObserver = getGlobalServiceContext()->getOpObserver();
                if (opObserver)
                    opObserver->onCreateIndex(txn, systemIndexes, specs[i]);
            }

            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createIndexes", ns.ns());

        result.append("numIndexesAfter", collection->getIndexCatalog()->numIndexesTotal(txn));

        lastOpSetterGuard.Dismiss();

        return true;
    }

private:
    static Status checkUniqueIndexConstraints(OperationContext* txn,
                                              StringData ns,
                                              const BSONObj& newIdxKey) {
        invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));

        auto metadata(CollectionShardingState::get(txn, ns.toString())->getMetadata());
        if (metadata) {
            ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
            if (!shardKeyPattern.isUniqueIndexCompatible(newIdxKey)) {
                return Status(ErrorCodes::CannotCreateIndex,
                              str::stream() << "cannot create unique index over " << newIdxKey
                                            << " with shard key pattern "
                                            << shardKeyPattern.toBSON());
            }
        }


        return Status::OK();
    }
} cmdCreateIndex;
}
