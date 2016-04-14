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
class RemoteCommandTargeter;

using ShardId = std::string;

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
     * Instantiates a new shard connection management object for the specified shard.
     */
    Shard(const ShardId& id,
          const ConnectionString& originalConnString,
          std::unique_ptr<RemoteCommandTargeter> targeter);

    ~Shard();

    const ShardId getId() const {
        return _id;
    }

    /**
     * Returns true if this shard object represents the config server.
     */
    bool isConfig() const;


    /**
     * Returns the current config string.
     */
    const ConnectionString getConnString() const;

    /**
     * Returns the config string that was used on the shard creation. The RS config string may be
     * different.
     */
    const ConnectionString originalConnString() const {
        return _originalConnString;
    }

    std::shared_ptr<RemoteCommandTargeter> getTargeter() const {
        return _targeter;
    }

    /**
     * Returns a string description of this shard entry.
     */
    std::string toString() const;


    /**
     * Notifies the RemoteCommandTargeter owned by the shard of a particular mode of failure for the
     * specified host.
     */
    void updateReplSetMonitor(const HostAndPort& remoteHost, const Status& remoteCommandStatus);


private:
    /**
     * Identifier of the shard as obtained from the configuration data (i.e. shard0000).
     */
    const ShardId _id;

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
