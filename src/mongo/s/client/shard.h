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

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/client/connection_string.h"

namespace mongo {

    class BSONObj;
    class RemoteCommandRunner;
    class RemoteCommandTargeter;

    using ShardId = std::string;

    /**
     * Contains runtime information obtained from the shard.
     */
    class ShardStatus {
    public:
        ShardStatus(long long dataSizeBytes, const std::string& version);

        long long dataSizeBytes() const { return _dataSizeBytes; }
        const std::string& mongoVersion() const { return _mongoVersion; }

        std::string toString() const;

        bool operator< (const ShardStatus& other) const;

    private:
        long long _dataSizeBytes;
        std::string _mongoVersion;
    };

    class Shard;
    using ShardPtr = std::shared_ptr<Shard>;

    /*
     * Maintains the targeting and command execution logic for a single shard. Performs polling of
     * the shard (if replica set).
     */
    class Shard {
        MONGO_DISALLOW_COPYING(Shard);
    public:
        /**
         * Instantiates a new shard connection management object for the specified shard and
         * connection string.
         */
        Shard(const ShardId& id, const ConnectionString& connStr);
        ~Shard();

        const ShardId& getId() const { return _id; }

        const ConnectionString& getConnString() const { return _cs; }

        RemoteCommandTargeter* getTargeter() const;

        RemoteCommandRunner* getCommandRunner() const;

        std::string toString() const {
            return _id + ":" + _cs.toString();
        }

        BSONObj runCommand(const std::string& db, const std::string& simple) const;
        BSONObj runCommand(const std::string& db, const BSONObj& cmd) const;

        bool runCommand(const std::string& db, const std::string& simple, BSONObj& res) const;
        bool runCommand(const std::string& db, const BSONObj& cmd, BSONObj& res) const;

        /**
         * Returns metadata and stats for this shard.
         */
        ShardStatus getStatus() const;

        static ShardPtr lookupRSName(const std::string& name);
        
        /**
         * @parm current - shard where the chunk/database currently lives in
         * @return the currently emptiest shard, if best then current, or nullptr
         */
        static ShardPtr pick();

        static void reloadShardInfo();

        static void removeShard(const ShardId& id);
        
        static void installShard(const ShardId& id, const Shard& shard);

    private:
        const ShardId _id;
        const ConnectionString _cs;
    };

} // namespace mongo
