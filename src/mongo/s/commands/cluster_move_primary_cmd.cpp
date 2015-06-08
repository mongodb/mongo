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

#include <boost/shared_ptr.hpp>
#include <set>

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

    using boost::shared_ptr;
    using std::set;
    using std::string;

namespace {

    class MoveDatabasePrimaryCommand : public Command {
    public:
        MoveDatabasePrimaryCommand() : Command("movePrimary", false, "moveprimary") { }

        virtual bool slaveOk() const {
            return true;
        }

        virtual bool adminOnly() const {
            return true;
        }

        virtual bool isWriteCommandForConfigServer() const {
            return false;
        }

        virtual void help(std::stringstream& help) const {
            help << " example: { moveprimary : 'foo' , to : 'localhost:9999' }";
        }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {

            if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                                                        ResourcePattern::forDatabaseName(
                                                                        parseNs(dbname, cmdObj)),
                                                        ActionType::moveChunk)) {
                return Status(ErrorCodes::Unauthorized, "Unauthorized");
            }

            return Status::OK();
        }

        virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
            return cmdObj.firstElement().str();
        }

        virtual bool run(OperationContext* txn,
                         const std::string& dbname_unused,
                         BSONObj& cmdObj,
                         int options,
                         std::string& errmsg,
                         BSONObjBuilder& result) {

            const string dbname = parseNs("", cmdObj);

            if (dbname.empty() || !nsIsDbOnly(dbname)) {
                errmsg = "invalid db name specified: " + dbname;
                return false;
            }

            if (dbname == "admin" || dbname == "config" || dbname == "local") {
                errmsg = "can't move primary for " + dbname + " database";
                return false;
            }

            // Flush all cached information. This can't be perfect, but it's better than nothing.
            grid.catalogCache()->invalidate(dbname);

            auto status = grid.catalogCache()->getDatabase(dbname);
            if (!status.isOK()) {
                return appendCommandStatus(result, status.getStatus());
            }

            shared_ptr<DBConfig> config = status.getValue();

            const string to = cmdObj["to"].valuestrsafe();
            if (!to.size()) {
                errmsg = "you have to specify where you want to move it";
                return false;
            }

            shared_ptr<Shard> toShard = grid.shardRegistry()->findIfExists(to);
            if (!toShard) {
                string msg(str::stream() << "Could not move database '" << dbname
                                         << "' to shard '" << to
                                         << "' because the shard does not exist");
                log() << msg;
                return appendCommandStatus(result,
                                           Status(ErrorCodes::ShardNotFound, msg));
            }

            shared_ptr<Shard> fromShard =
                grid.shardRegistry()->findIfExists(config->getPrimaryId());
            invariant(fromShard);

            if (fromShard->getConnString().sameLogicalEndpoint(toShard->getConnString())) {
                errmsg = "it is already the primary";
                return false;
            }

            if (!grid.catalogManager()->isShardHost(toShard->getConnString())) {
                errmsg = "that server isn't known to me";
                return false;
            }

            log() << "Moving " << dbname << " primary from: "
                  << fromShard->toString() << " to: " << toShard->toString();

            string whyMessage(str::stream() << "Moving primary shard of " << dbname);
            auto scopedDistLock = grid.catalogManager()->getDistLockManager()->lock(
                    dbname + "-movePrimary", whyMessage);

            if (!scopedDistLock.isOK()) {
                return appendCommandStatus(result, scopedDistLock.getStatus());
            }

            set<string> shardedColls;
            config->getAllShardedCollections(shardedColls);

            // Record start in changelog
            BSONObj moveStartDetails = _buildMoveEntry(dbname,
                                                       fromShard->toString(),
                                                       toShard->toString(),
                                                       shardedColls);

            grid.catalogManager()->logChange(txn, "movePrimary.start", dbname, moveStartDetails);

            BSONArrayBuilder barr;
            barr.append(shardedColls);

            ScopedDbConnection toconn(toShard->getConnString());

            // TODO ERH - we need a clone command which replays operations from clone start to now
            //            can just use local.oplog.$main
            BSONObj cloneRes;
            bool worked = toconn->runCommand(
                            dbname.c_str(),
                            BSON("clone" << fromShard->getConnString().toString()
                                         << "collsToIgnore" << barr.arr()
                                         << bypassDocumentValidationCommandOption() << true),
                            cloneRes);
            toconn.done();

            if (!worked) {
                log() << "clone failed" << cloneRes;
                errmsg = "clone failed";
                return false;
            }

            const string oldPrimary = fromShard->getConnString().toString();

            ScopedDbConnection fromconn(fromShard->getConnString());

            config->setPrimary(toShard->getConnString().toString());

            if (shardedColls.empty()){

                // TODO: Collections can be created in the meantime, and we should handle in the future.
                log() << "movePrimary dropping database on " << oldPrimary
                      << ", no sharded collections in " << dbname;

                try {
                    fromconn->dropDatabase(dbname.c_str());
                }
                catch (DBException& e){
                    e.addContext(str::stream() << "movePrimary could not drop the database "
                                               << dbname << " on " << oldPrimary);
                    throw;
                }

            }
            else if (cloneRes["clonedColls"].type() != Array) {
                // Legacy behavior from old mongod with sharded collections, *do not* delete
                // database, but inform user they can drop manually (or ignore).
                warning() << "movePrimary legacy mongod behavior detected. "
                          << "User must manually remove unsharded collections in database "
                          << dbname << " on " << oldPrimary;

            }
            else {
                // We moved some unsharded collections, but not all
                BSONObjIterator it(cloneRes["clonedColls"].Obj());

                while (it.more()){
                    BSONElement el = it.next();
                    if (el.type() == String){
                        try {
                            log() << "movePrimary dropping cloned collection " << el.String()
                                  << " on " << oldPrimary;
                            fromconn->dropCollection(el.String());
                        }
                        catch (DBException& e){
                            e.addContext(str::stream() << "movePrimary could not drop the cloned collection "
                                                       << el.String() << " on " << oldPrimary);
                            throw;
                        }
                    }
                }
            }

            fromconn.done();

            result << "primary" << toShard->toString();

            // Record finish in changelog
            BSONObj moveFinishDetails = _buildMoveEntry(dbname,
                                                        oldPrimary,
                                                        toShard->toString(),
                                                        shardedColls);

            grid.catalogManager()->logChange(txn, "movePrimary", dbname, moveFinishDetails);
            return true;
        }

    private:
        static BSONObj _buildMoveEntry(const string db,
                                       const string from,
                                       const string to,
                                       set<string> shardedColls) {

            BSONObjBuilder details;
            details.append("database", db);
            details.append("from", from);
            details.append("to", to);

            BSONArrayBuilder collB(details.subarrayStart("shardedCollections"));
            {
                set<string>::iterator it;
                for (it = shardedColls.begin(); it != shardedColls.end(); ++it) {
                    collB.append(*it);
                }
            }
            collB.done();

            return details.obj();
        }

    } movePrimary;

} // namespace
} // namespace mongo
