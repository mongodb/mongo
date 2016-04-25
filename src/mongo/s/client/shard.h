/**
 * Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"

namespace mongo {

class OperationContext;
class RemoteCommandTargeter;

using ShardId = std::string;

/**
 * Presents an interface for talking to shards, regardless of whether that shard is remote or is
 * the current (local) shard.
 */
class Shard {
public:
    struct CommandResponse {
        CommandResponse(BSONObj _response,
                        BSONObj _metadata,
                        Status _commandStatus,
                        Status _writeConcernStatus)
            : response(std::move(_response)),
              metadata(std::move(_metadata)),
              commandStatus(std::move(_commandStatus)),
              writeConcernStatus(std::move(_writeConcernStatus)) {}
        BSONObj response;
        BSONObj metadata;
        Status commandStatus;
        Status writeConcernStatus;  // Only valid to check when commandStatus is OK.
    };

    struct QueryResponse {
        std::vector<BSONObj> docs;
        repl::OpTime opTime;
    };

    enum class RetryPolicy {
        kIdempotent,
        kNotIdempotent,
        kNoRetry,
    };

    virtual ~Shard() = default;

    const ShardId getId() const;

    /**
     * Returns true if this shard object represents the config server.
     */
    bool isConfig() const;

    /**
     * Returns the current connection string for the shard.
     *
     * This is only valid to call on ShardRemote instances.
     */
    virtual const ConnectionString getConnString() const = 0;

    /**
     * Returns the connection string that was used when the shard was added. The current connection
     * string may be different for shards that are replica sets.
     *
     * This is only valid to call on ShardRemote instances.
     */
    virtual const ConnectionString originalConnString() const = 0;

    /**
     * Returns the RemoteCommandTargeter for the hosts in this shard.
     *
     * This is only valid to call on ShardRemote instances.
     */
    virtual std::shared_ptr<RemoteCommandTargeter> getTargeter() const = 0;

    /**
     * Notifies the RemoteCommandTargeter owned by the shard of a particular mode of failure for
     * the specified host.
     *
     * This is only valid to call on ShardRemote instances.
     */
    virtual void updateReplSetMonitor(const HostAndPort& remoteHost,
                                      const Status& remoteCommandStatus) = 0;

    /**
     * Returns a string description of this shard entry.
     */
    virtual std::string toString() const = 0;

    /**
     * Runs a command against this shard and returns the BSON command response, as well as the
     * already-parsed out Status of the command response and write concern error (if present).
     * Retries failed operations according to the given "retryPolicy".
     */
    StatusWith<CommandResponse> runCommand(OperationContext* txn,
                                           const ReadPreferenceSetting& readPref,
                                           const std::string& dbName,
                                           const BSONObj& cmdObj,
                                           const BSONObj& metadata,
                                           RetryPolicy retryPolicy);

    /**
    * Warning: This method exhausts the cursor and pulls all data into memory.
    * Do not use other than for very small (i.e., admin or metadata) collections.
    * Performs retries if the query fails in accordance with the kIdempotent RetryPolicy.
    */
    StatusWith<QueryResponse> exhaustiveFindOnConfig(OperationContext* txn,
                                                     const ReadPreferenceSetting& readPref,
                                                     const NamespaceString& nss,
                                                     const BSONObj& query,
                                                     const BSONObj& sort,
                                                     const boost::optional<long long> limit);

protected:
    Shard(const ShardId& id);

private:
    /**
     * Returns whether a server operation which failed with the given error code should be retried
     * (i.e. is safe to retry and has the potential to succeed next time).  The 'options' argument
     * describes whether the operation that generated the given code was idempotent, which affects
     * which codes are safe to retry on.
     */
    virtual bool _isRetriableError(ErrorCodes::Error code, RetryPolicy options) = 0;

    virtual StatusWith<CommandResponse> _runCommand(OperationContext* txn,
                                                    const ReadPreferenceSetting& readPref,
                                                    const std::string& dbname,
                                                    const BSONObj& cmdObj,
                                                    const BSONObj& metadata) = 0;

    virtual StatusWith<QueryResponse> _exhaustiveFindOnConfig(OperationContext* txn,
                                                              const ReadPreferenceSetting& readPref,
                                                              const NamespaceString& nss,
                                                              const BSONObj& query,
                                                              const BSONObj& sort,
                                                              boost::optional<long long> limit) = 0;

    /**
     * Identifier of the shard as obtained from the configuration data (i.e. shard0000).
     */
    const ShardId _id;
};

}  // namespace mongo
