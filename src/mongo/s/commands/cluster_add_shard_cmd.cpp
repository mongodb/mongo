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

#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/commands.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/add_shard_request_type.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};
const char kShardAdded[] = "shardAdded";

class AddShardCmd : public Command {
public:
    AddShardCmd() : Command("addShard", false, "addshard") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(std::stringstream& help) const {
        help << "add a new shard to the system";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::addShard);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        auto parsedRequest = uassertStatusOK(AddShardRequest::parseFromMongosCommand(cmdObj));

        auto configShard = Grid::get(txn)->shardRegistry()->getConfigShard();
        auto cmdResponseStatus =
            uassertStatusOK(configShard->runCommand(txn,
                                                    kPrimaryOnlyReadPreference,
                                                    "admin",
                                                    parsedRequest.toCommandForConfig(),
                                                    Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(cmdResponseStatus.commandStatus);

        string shardAdded;
        uassertStatusOK(
            bsonExtractStringField(cmdResponseStatus.response, kShardAdded, &shardAdded));
        result << "shardAdded" << shardAdded;

        // Ensure the added shard is visible to this process.
        auto shardRegistry = Grid::get(txn)->shardRegistry();
        if (!shardRegistry->getShard(txn, shardAdded)) {
            return appendCommandStatus(result,
                                       {ErrorCodes::OperationFailed,
                                        "Could not find shard metadata for shard after adding it. "
                                        "This most likely indicates that the shard was removed "
                                        "immediately after it was added."});
        }

        return true;
    }

} addShard;

}  // namespace
}  // namespace mongo
