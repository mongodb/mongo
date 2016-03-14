/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/d_state.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::stringstream;

namespace {

class UnsetShardingCommand : public Command {
public:
    UnsetShardingCommand() : Command("unsetSharding") {}

    virtual void help(stringstream& help) const {
        help << "internal";
    }


    virtual bool adminOnly() const {
        return true;
    }

    virtual bool slaveOk() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        ShardedConnectionInfo::reset(txn->getClient());
        return true;
    }

} unsetShardingCommand;

class GetShardVersion : public Command {
public:
    GetShardVersion() : Command("getShardVersion") {}

    virtual void help(stringstream& help) const {
        help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }


    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::getShardVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const NamespaceString nss(cmdObj["getShardVersion"].valuestrsafe());
        if (!nss.isValid()) {
            uasserted(ErrorCodes::InvalidNamespace, "Command requires valid collection namespace");
        }

        ShardingState* const shardingState = ShardingState::get(txn);

        if (shardingState->enabled()) {
            result.append("configServer", shardingState->getConfigServer(txn).toString());
        } else {
            result.append("configServer", "");
        }

        AutoGetCollection autoColl(txn, nss, MODE_IS);
        CollectionShardingState* const css = CollectionShardingState::get(txn, nss);

        shared_ptr<CollectionMetadata> metadata(css ? css->getMetadata() : nullptr);
        if (metadata) {
            result.appendTimestamp("global", metadata->getShardVersion().toLong());
        } else {
            result.appendTimestamp("global", ChunkVersion(0, 0, OID()).toLong());
        }

        ShardedConnectionInfo* const info = ShardedConnectionInfo::get(txn->getClient(), false);
        result.appendBool("inShardedMode", info != NULL);
        if (info) {
            result.appendTimestamp("mine", info->getVersion(nss.ns()).toLong());
        } else {
            result.appendTimestamp("mine", 0);
        }

        if (cmdObj["fullMetadata"].trueValue()) {
            shared_ptr<CollectionMetadata> metadata =
                shardingState->getCollectionMetadata(nss.ns());
            if (metadata) {
                result.append("metadata", metadata->toBSON());
            } else {
                result.append("metadata", BSONObj());
            }
        }

        return true;
    }

} getShardVersionCmd;

class ShardingStateCmd : public Command {
public:
    ShardingStateCmd() : Command("shardingState") {}


    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::shardingState);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        ShardingState::get(txn)->appendInfo(txn, result);
        return true;
    }

} shardingStateCmd;

}  // namespace

bool haveLocalShardingInfo(OperationContext* txn, const string& ns) {
    if (!ShardingState::get(txn)->enabled()) {
        return false;
    }

    const auto& oss = OperationShardingState::get(txn);
    if (oss.hasShardVersion()) {
        return true;
    }

    const auto& sci = ShardedConnectionInfo::get(txn->getClient(), false);
    if (sci && !sci->getVersion(ns).isStrictlyEqualTo(ChunkVersion::UNSHARDED())) {
        return true;
    }

    return false;
}

void usingAShardConnection(const string& addr) {}

void saveGLEStats(const BSONObj& result, StringData hostString) {}

}  // namespace mongo
