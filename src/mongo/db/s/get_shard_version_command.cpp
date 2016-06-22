/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::shared_ptr;

namespace {

class GetShardVersion : public Command {
public:
    GetShardVersion() : Command("getShardVersion") {}

    void help(std::stringstream& help) const override {
        help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::getShardVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << nss.ns() << " is not a valid namespace",
                nss.isValid());

        ShardingState* const gss = ShardingState::get(txn);
        if (gss->enabled()) {
            result.append("configServer", gss->getConfigServer(txn).toString());
        } else {
            result.append("configServer", "");
        }

        ShardedConnectionInfo* const sci = ShardedConnectionInfo::get(txn->getClient(), false);
        result.appendBool("inShardedMode", sci != nullptr);
        if (sci) {
            result.appendTimestamp("mine", sci->getVersion(nss.ns()).toLong());
        } else {
            result.appendTimestamp("mine", 0);
        }

        AutoGetCollection autoColl(txn, nss, MODE_IS);
        CollectionShardingState* const css = CollectionShardingState::get(txn, nss);

        ScopedCollectionMetadata metadata;
        if (css) {
            metadata = css->getMetadata();
        }

        if (metadata) {
            result.appendTimestamp("global", metadata->getShardVersion().toLong());
        } else {
            result.appendTimestamp("global", ChunkVersion(0, 0, OID()).toLong());
        }

        if (cmdObj["fullMetadata"].trueValue()) {
            if (metadata) {
                result.append("metadata", metadata->toBSON());
            } else {
                result.append("metadata", BSONObj());
            }
        }

        return true;
    }

} getShardVersionCmd;

}  // namespace
}  // namespace mongo
