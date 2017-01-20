/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::set;
using std::string;

namespace {

class MoveDatabasePrimaryCommand : public Command {
public:
    MoveDatabasePrimaryCommand() : Command("movePrimary", false, "moveprimary") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void help(std::stringstream& help) const {
        help << " example: { moveprimary : 'foo' , to : 'localhost:9999' }";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(parseNs(dbname, cmdObj)), ActionType::moveChunk)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        const auto nsElt = cmdObj.firstElement();
        uassert(ErrorCodes::InvalidNamespace,
                "'movePrimary' must be of type String",
                nsElt.type() == BSONType::String);
        return nsElt.str();
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname_unused,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const string dbname = parseNs("", cmdObj);

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbname,
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        if (dbname == NamespaceString::kAdminDb || dbname == NamespaceString::kConfigDb ||
            dbname == NamespaceString::kLocalDb) {
            errmsg = "can't move primary for " + dbname + " database";
            return false;
        }

        auto const catalogClient = Grid::get(txn)->catalogClient(txn);
        auto const catalogCache = Grid::get(txn)->catalogCache();
        auto const shardRegistry = Grid::get(txn)->shardRegistry();

        // Flush all cached information. This can't be perfect, but it's better than nothing.
        catalogCache->invalidate(dbname);

        auto config = uassertStatusOK(catalogCache->getDatabase(txn, dbname));

        const auto toElt = cmdObj["to"];
        uassert(ErrorCodes::TypeMismatch,
                "'to' must be of type String",
                toElt.type() == BSONType::String);
        const std::string to = toElt.str();
        if (!to.size()) {
            errmsg = "you have to specify where you want to move it";
            return false;
        }

        const auto fromShard =
            uassertStatusOK(shardRegistry->getShard(txn, config->getPrimaryId()));

        const auto toShard = [&]() {
            auto toShardStatus = shardRegistry->getShard(txn, to);
            if (!toShardStatus.isOK()) {
                const std::string msg(
                    str::stream() << "Could not move database '" << dbname << "' to shard '" << to
                                  << "' due to "
                                  << toShardStatus.getStatus().reason());
                log() << msg;
                uasserted(toShardStatus.getStatus().code(), msg);
            }

            return toShardStatus.getValue();
        }();

        uassert(ErrorCodes::IllegalOperation,
                "it is already the primary",
                fromShard->getId() != toShard->getId());

        log() << "Moving " << dbname << " primary from: " << fromShard->toString()
              << " to: " << toShard->toString();

        const std::string whyMessage(str::stream() << "Moving primary shard of " << dbname);
        auto scopedDistLock = uassertStatusOK(catalogClient->getDistLockManager()->lock(
            txn, dbname + "-movePrimary", whyMessage, DistLockManager::kDefaultLockTimeout));

        const auto shardedColls = getAllShardedCollectionsForDb(txn, dbname);

        // Record start in changelog
        catalogClient->logChange(
            txn,
            "movePrimary.start",
            dbname,
            _buildMoveLogEntry(dbname, fromShard->toString(), toShard->toString(), shardedColls),
            ShardingCatalogClient::kMajorityWriteConcern);

        ScopedDbConnection toconn(toShard->getConnString());

        // TODO ERH - we need a clone command which replays operations from clone start to now
        //            can just use local.oplog.$main
        BSONObj cloneRes;
        bool hasWCError = false;

        {
            BSONArrayBuilder barr;
            for (const auto& shardedColl : shardedColls) {
                barr.append(shardedColl.ns());
            }

            const bool worked = toconn->runCommand(
                dbname,
                BSON("clone" << fromShard->getConnString().toString() << "collsToIgnore"
                             << barr.arr()
                             << bypassDocumentValidationCommandOption()
                             << true
                             << "writeConcern"
                             << txn->getWriteConcern().toBSON()),
                cloneRes);
            toconn.done();

            if (!worked) {
                log() << "clone failed" << redact(cloneRes);
                errmsg = "clone failed";
                return false;
            }

            if (auto wcErrorElem = cloneRes["writeConcernError"]) {
                appendWriteConcernErrorToCmdResponse(toShard->getId(), wcErrorElem, result);
                hasWCError = true;
            }
        }

        // Update the new primary in the config server metadata
        {
            auto dbt = uassertStatusOK(catalogClient->getDatabase(txn, dbname)).value;
            dbt.setPrimary(toShard->getId());

            uassertStatusOK(catalogClient->updateDatabase(txn, dbname, dbt));
        }

        // Ensure the next attempt to retrieve the database or any of its collections will do a full
        // reload
        catalogCache->invalidate(dbname);

        const string oldPrimary = fromShard->getConnString().toString();

        ScopedDbConnection fromconn(fromShard->getConnString());

        if (shardedColls.empty()) {
            // TODO: Collections can be created in the meantime, and we should handle in the future.
            log() << "movePrimary dropping database on " << oldPrimary
                  << ", no sharded collections in " << dbname;

            try {
                BSONObj dropDBInfo;
                fromconn->dropDatabase(dbname.c_str(), txn->getWriteConcern(), &dropDBInfo);
                if (!hasWCError) {
                    if (auto wcErrorElem = dropDBInfo["writeConcernError"]) {
                        appendWriteConcernErrorToCmdResponse(
                            fromShard->getId(), wcErrorElem, result);
                        hasWCError = true;
                    }
                }
            } catch (DBException& e) {
                e.addContext(str::stream() << "movePrimary could not drop the database " << dbname
                                           << " on "
                                           << oldPrimary);
                throw;
            }

        } else if (cloneRes["clonedColls"].type() != Array) {
            // Legacy behavior from old mongod with sharded collections, *do not* delete
            // database, but inform user they can drop manually (or ignore).
            warning() << "movePrimary legacy mongod behavior detected. "
                      << "User must manually remove unsharded collections in database " << dbname
                      << " on " << oldPrimary;
        } else {
            // We moved some unsharded collections, but not all
            BSONObjIterator it(cloneRes["clonedColls"].Obj());

            while (it.more()) {
                BSONElement el = it.next();
                if (el.type() == String) {
                    try {
                        log() << "movePrimary dropping cloned collection " << el.String() << " on "
                              << oldPrimary;
                        BSONObj dropCollInfo;
                        fromconn->dropCollection(
                            el.String(), txn->getWriteConcern(), &dropCollInfo);
                        if (!hasWCError) {
                            if (auto wcErrorElem = dropCollInfo["writeConcernError"]) {
                                appendWriteConcernErrorToCmdResponse(
                                    fromShard->getId(), wcErrorElem, result);
                                hasWCError = true;
                            }
                        }

                    } catch (DBException& e) {
                        e.addContext(str::stream()
                                     << "movePrimary could not drop the cloned collection "
                                     << el.String()
                                     << " on "
                                     << oldPrimary);
                        throw;
                    }
                }
            }
        }

        fromconn.done();

        result << "primary" << toShard->toString();

        // Record finish in changelog
        catalogClient->logChange(
            txn,
            "movePrimary",
            dbname,
            _buildMoveLogEntry(dbname, oldPrimary, toShard->toString(), shardedColls),
            ShardingCatalogClient::kMajorityWriteConcern);

        return true;
    }

private:
    static BSONObj _buildMoveLogEntry(const std::string& db,
                                      const std::string& from,
                                      const std::string& to,
                                      const std::vector<NamespaceString>& shardedColls) {
        BSONObjBuilder details;
        details.append("database", db);
        details.append("from", from);
        details.append("to", to);

        BSONArrayBuilder collB(details.subarrayStart("shardedCollections"));
        for (const auto& shardedColl : shardedColls) {
            collB.append(shardedColl.ns());
        }
        collB.done();

        return details.obj();
    }

} clusterMovePrimaryCmd;

}  // namespace
}  // namespace mongo
