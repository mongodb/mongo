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

#include "mongo/base/disallow_copying.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class ShardLocal : public Shard {
    MONGO_DISALLOW_COPYING(ShardLocal);

public:
    explicit ShardLocal(const ShardId& id) : Shard(id) {}

    ~ShardLocal() = default;

    /**
     * These functions are implemented for the Shard interface's sake. They should not be called on
     * ShardLocal because doing so triggers invariants.
     */
    const ConnectionString getConnString() const override;
    const ConnectionString originalConnString() const override;
    std::shared_ptr<RemoteCommandTargeter> getTargeter() const override;
    void updateReplSetMonitor(const HostAndPort& remoteHost,
                              const Status& remoteCommandStatus) override;

    std::string toString() const override;

    bool isRetriableError(ErrorCodes::Error code, RetryPolicy options) final;

private:
    StatusWith<Shard::CommandResponse> _runCommand(OperationContext* txn,
                                                   const ReadPreferenceSetting& unused,
                                                   const std::string& dbName,
                                                   const BSONObj& cmdObj) final;

    StatusWith<Shard::QueryResponse> _exhaustiveFindOnConfig(
        OperationContext* txn,
        const ReadPreferenceSetting& readPref,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit) final;
};

}  // namespace mongo
