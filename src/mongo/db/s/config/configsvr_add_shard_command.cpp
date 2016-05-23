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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

using std::string;
using str::stream;

namespace {

const char kShardName[] = "name";
const char kMaxSizeMB[] = "maxSize";

const long long kMaxSizeMBDefault = 0;

}  // namespace

class AddShardRequest {
public:
    /**
     * Parses the provided BSON content as the internal _configsvrAddShard command, and if it is
     * correct, constructs an AddShardRequest object from it.
     */
    static StatusWith<AddShardRequest> parseFromConfigCommand(const BSONObj& obj) {
        // required fields

        auto swConnString = ConnectionString::parse(obj.firstElement().valuestrsafe());
        if (!swConnString.isOK()) {
            return swConnString.getStatus();
        }
        ConnectionString connString = std::move(swConnString.getValue());

        AddShardRequest request(std::move(connString));

        // optional fields

        {
            string name;
            Status status = bsonExtractStringField(obj, kShardName, &name);
            if (status.isOK()) {
                request._name = std::move(name);
            } else if (status != ErrorCodes::NoSuchKey) {
                return status;
            }
        }
        {
            long long maxSizeMB;
            Status status = bsonExtractIntegerField(obj, kMaxSizeMB, &maxSizeMB);
            if (status.isOK()) {
                request._maxSizeMB = std::move(maxSizeMB);
            } else if (status != ErrorCodes::NoSuchKey) {
                return status;
            }
        }

        return request;
    }

    Status validate(bool allowLocalHost) {
        // Check that if one of the new shard's hosts is localhost, we are allowed to use localhost
        // as a hostname. (Using localhost requires that every server in the cluster uses
        // localhost).
        std::vector<HostAndPort> serverAddrs = _connString.getServers();
        for (size_t i = 0; i < serverAddrs.size(); i++) {
            if (serverAddrs[i].isLocalHost() != allowLocalHost) {
                string errmsg = str::stream()
                    << "Can't use localhost as a shard since all shards need to"
                    << " communicate. Either use all shards and configdbs in localhost"
                    << " or all in actual IPs. host: " << serverAddrs[i].toString()
                    << " isLocalHost:" << serverAddrs[i].isLocalHost();

                log() << "addshard request " << toString()
                      << " failed: attempt to mix localhosts and IPs";
                return Status(ErrorCodes::InvalidOptions, errmsg);
            }

            // If the server has no port, assign it the default port.
            if (!serverAddrs[i].hasPort()) {
                serverAddrs[i] =
                    HostAndPort(serverAddrs[i].host(), ServerGlobalParams::ShardServerPort);
            }
        }
        return Status::OK();
    }

    string toString() const {
        stream ss;
        ss << "AddShardRequest shard: " << _connString.toString();
        if (hasName())
            ss << ", name: " << *_name;
        if (hasMaxSize())
            ss << ", maxSize: " << *_maxSizeMB;
        return ss;
    }

    const ConnectionString& getConnString() const {
        return _connString;
    }

    bool hasName() const {
        return _name.is_initialized();
    }

    const string& getName() const {
        invariant(_name.is_initialized());
        return *_name;
    }

    bool hasMaxSize() const {
        return _maxSizeMB.is_initialized();
    }

    long long getMaxSize() const {
        invariant(_maxSizeMB.is_initialized());
        return *_maxSizeMB;
    }

private:
    explicit AddShardRequest(ConnectionString connString) : _connString(std::move(connString)) {}

    // If the shard to be added is standalone, then the hostname and port of the mongod instance to
    // be added. If the shard to be added is a replica set, the name of the replica set and the
    // hostname and port of at least one member of the replica set.
    ConnectionString _connString;

    // A name for the shard. If not specified, a unique name is automatically generated.
    boost::optional<string> _name;

    // The maximum size in megabytes of the shard. If set to 0, the size is not limited.
    boost::optional<long long> _maxSizeMB;
};

/**
 * Internal sharding command run on config servers to add a shard to the cluster.
 */
class ConfigSvrAddShardCommand : public Command {
public:
    ConfigSvrAddShardCommand() : Command("_configsvrAddShard") {}

    void help(std::stringstream& help) const override {
        help << "Internal command, which is exported by the sharding config server. Do not call "
                "directly. Validates and adds a new shard to the cluster.";
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* txn,
             const std::string& unusedDbName,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::IllegalOperation,
                       "_configsvrAddShard can only be run on config servers"));
        }

        auto swParsedRequest = AddShardRequest::parseFromConfigCommand(cmdObj);
        if (!swParsedRequest.isOK()) {
            return appendCommandStatus(result, swParsedRequest.getStatus());
        }
        auto parsedRequest = std::move(swParsedRequest.getValue());

        auto replCoord = repl::ReplicationCoordinator::get(txn);
        auto rsConfig = replCoord->getConfig();

        auto validationStatus = parsedRequest.validate(rsConfig.isLocalHostAllowed());
        if (!validationStatus.isOK()) {
            return appendCommandStatus(result, validationStatus);
        }

        audit::logAddShard(ClientBasic::getCurrent(),
                           parsedRequest.hasName() ? parsedRequest.getName() : "",
                           parsedRequest.getConnString().toString(),
                           parsedRequest.hasMaxSize() ? parsedRequest.getMaxSize()
                                                      : kMaxSizeMBDefault);

        StatusWith<string> addShardResult = grid.catalogManager(txn)->addShard(
            txn,
            parsedRequest.hasName() ? &parsedRequest.getName() : nullptr,
            parsedRequest.getConnString(),
            parsedRequest.hasMaxSize() ? parsedRequest.getMaxSize() : kMaxSizeMBDefault);

        if (!addShardResult.isOK()) {
            log() << "addShard request '" << parsedRequest << "'"
                  << "failed" << causedBy(addShardResult.getStatus());
            return appendCommandStatus(result, addShardResult.getStatus());
        }

        result << "shardAdded" << addShardResult.getValue();

        return true;
    }
} configsvrAddShardCmd;

}  // namespace
}  // namespace mongo
