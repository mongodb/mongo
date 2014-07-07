/**
 * Copyright (C) 2013 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_documents_update_guard.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/commands/user_management_commands.h"
#include "mongo/db/repl/multicmd.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/version.h"

namespace mongo {
namespace {

    Status checkReplicaMemberVersions() {

        // TODO(spencer): SERVER-14460 Resurrect or remove this function.
//        if (repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
//                repl::ReplicationCoordinator::modeReplSet)
//            return Status::OK();
//
//
//        std::list<repl::Target> rsMembers;
//        try {
//            const unsigned rsSelfId = repl::theReplSet->selfId();
//            const std::vector<repl::ReplSetConfig::MemberCfg>& rsMemberConfigs =
//                repl::theReplSet->config().members;
//            for (size_t i = 0; i < rsMemberConfigs.size(); ++i) {
//                const unsigned otherId = rsMemberConfigs[i]._id;
//                if (rsSelfId == otherId)
//                    continue;
//                const repl::Member* other = repl::theReplSet->findById(otherId);
//                if (!other) {
//                    log() << "During authSchemaUpgrade, no information about replica set member "
//                        "with id " << otherId << "; ignoring.";
//                    continue;
//                }
//                if (!other->hbinfo().maybeUp()) {
//                    log() << "During authSchemaUpgrade, replica set member " << other->h() <<
//                        " is down; ignoring.";
//                    continue;
//                }
//                rsMembers.push_back(repl::Target(other->fullName()));
//            }
//
//            multiCommand(BSON("buildInfo" << 1), rsMembers);
//        }
//        catch (const DBException& ex) {
//            return ex.toStatus();
//        }
//
//        for (std::list<repl::Target>::const_iterator iter = rsMembers.begin();
//             iter != rsMembers.end();
//             ++iter) {
//
//            if (!iter->ok) {
//                logger::LogstreamBuilder wlog = warning();
//                wlog << "During authSchemaUpgrade, could not run buildInfo command on " <<
//                    iter->toHost;
//                if (!iter->result.isEmpty())
//                    wlog << "; response was " << iter->result.toString();
//                wlog << "; ignoring.";
//                continue;
//            }
//
//            const char* version = iter->result["version"].valuestrsafe();
//            if (!*version) {
//                return Status(ErrorCodes::RemoteValidationError, mongoutils::str::stream() <<
//                              "Missing or non-string \"version\" field in result of buildInfo "
//                              "command sent to " << iter->toHost << "; found " <<
//                              iter->result["version"]);
//            }
//
//            if (!isSameMajorVersion(version)) {
//                BSONArray foundVersionArray = toVersionArray(version);
//                return Status(ErrorCodes::RemoteValidationError, mongoutils::str::stream() <<
//                              "To upgrade auth schema in a replica set, all members must be "
//                              "running the same release series of mongod; found " <<
//                              foundVersionArray["0"] << '.' << foundVersionArray["1"] <<
//                              " on host  " << iter->toHost << " but expected " <<
//                              versionArray["0"] << '.' << versionArray["1"]);
//            }
//        }
        return Status(ErrorCodes::InternalError,
                      "Auth schema upgrade check for replica sets not implemented");
    }

    class CmdAuthSchemaUpgradeD : public CmdAuthSchemaUpgrade {
        virtual bool run(
                OperationContext* txn,
                const string& dbname,
                BSONObj& cmdObj,
                int options,
                string& errmsg,
                BSONObjBuilder& result,
                bool fromRepl) {

            int maxSteps;
            bool upgradeShardServers;
            BSONObj writeConcern;
            Status status = auth::parseAuthSchemaUpgradeStepCommand(
                    cmdObj,
                    dbname,
                    &maxSteps,
                    &upgradeShardServers,
                    &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();

            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("auth schema upgrade")) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            status = checkReplicaMemberVersions();
            if (!status.isOK())
                return appendCommandStatus(result, status);

            status = authzManager->upgradeSchema(txn, maxSteps, writeConcern);
            if (status.isOK())
                result.append("done", true);
            return appendCommandStatus(result, status);
        }

    } cmdAuthSchemaUpgradeStep;

}  // namespace
}  // namespace mongo
