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

#pragma once

#include <string>

#include "mongo/s/client/shard.h"

#include "mongo/base/disallow_copying.h"

namespace mongo {

/*
 * Maintains the targeting and command execution logic for a single shard. Performs polling of
 * the shard (if replica set).
 */
class ShardRemote : public Shard {
    MONGO_DISALLOW_COPYING(ShardRemote);

public:
    /**
     * Instantiates a new shard connection management object for the specified shard.
     */
    ShardRemote(const ShardId& id,
                const ConnectionString& originalConnString,
                std::unique_ptr<RemoteCommandTargeter> targeter);

    ~ShardRemote();

    const ConnectionString getConnString() const override;

    const ConnectionString originalConnString() const override {
        return _originalConnString;
    }

    std::shared_ptr<RemoteCommandTargeter> getTargeter() const override {
        return _targeter;
    }

    void updateReplSetMonitor(const HostAndPort& remoteHost,
                              const Status& remoteCommandStatus) override;

    std::string toString() const override;

    bool isRetriableError(ErrorCodes::Error code, RetryPolicy options) final;

    Status createIndexOnConfig(OperationContext* txn,
                               const NamespaceString& ns,
                               const BSONObj& keys,
                               bool unique) override;

private:
    /**
     * Returns the metadata that should be used when running commands against this shard with
     * the given read preference.
     *
     * NOTE: This method returns a reference to a constant defined in shard_remote.cpp.  Be careful
     * to never change it to return a reference to a temporary.
     */
    const BSONObj& _getMetadataForCommand(const ReadPreferenceSetting& readPref);

    Shard::HostWithResponse _runCommand(OperationContext* txn,
                                        const ReadPreferenceSetting& readPref,
                                        const std::string& dbname,
                                        const BSONObj& cmdObj) final;

    StatusWith<QueryResponse> _exhaustiveFindOnConfig(
        OperationContext* txn,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcernLevel,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit) final;

    /**
     * Connection string for the shard at the creation time.
     */
    const ConnectionString _originalConnString;

    /**
     * Targeter for obtaining hosts from which to read or to which to write.
     */
    const std::shared_ptr<RemoteCommandTargeter> _targeter;
};

}  // namespace mongo
