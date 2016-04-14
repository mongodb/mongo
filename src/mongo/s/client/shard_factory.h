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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"

namespace mongo {

class Shard;
class ConnectionString;

/**
 * An interface to Shard instantiating factory.
 */
class ShardFactory {
    MONGO_DISALLOW_COPYING(ShardFactory);

public:
    virtual ~ShardFactory() = default;

    /**
     * Creates a unique_ptr with a new instance of a Shard with the provided shardId
     * and connection string. This method is currently only used when shard does not exists yet.
     * Consider using createShard instead.
     * TODO: currently isLocal argument is ignored until the Shard will provide local and remote
     * implementations.
     */
    virtual std::unique_ptr<Shard> createUniqueShard(const std::string& shardId,
                                                     const ConnectionString& connStr,
                                                     bool isLocal) = 0;

    /**
     * Creates a shared_ptr with a new instance of a Shard with the provided shardId
     * and connection string.
     * TODO: currently isLocal argument is ignored until the Shard will provide local and remote
     * implementations.
     */
    virtual std::shared_ptr<Shard> createShard(const std::string& shardId,
                                               const ConnectionString& connStr,
                                               bool isLocal) = 0;

protected:
    ShardFactory() = default;
};

}  // namespace mongo
