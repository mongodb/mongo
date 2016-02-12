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

#include "mongo/db/write_concern_options.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
template <typename T>
class StatusWith;

/**
 * Returns the default write concern for migration cleanup on the donor shard and for cloning
 * documents on the destination shard.
 */
class ChunkMoveWriteConcernOptions {
public:
    /**
     * Parses the chunk move options expecting the format, which mongod should be getting and if
     * none is available, assigns defaults.
     */
    static StatusWith<ChunkMoveWriteConcernOptions> initFromCommand(const BSONObj& obj);

    /**
     * Returns the throttle options to be used when committing migrated documents on the recipient
     * shard's seconary.
     */
    const BSONObj& getSecThrottle() const {
        return _secThrottleObj;
    }

    /**
     * Returns the write concern options.
     */
    const WriteConcernOptions& getWriteConcern() const {
        return _writeConcernOptions;
    }

private:
    ChunkMoveWriteConcernOptions(BSONObj secThrottleObj, WriteConcernOptions writeConcernOptions);

    BSONObj _secThrottleObj;
    WriteConcernOptions _writeConcernOptions;
};

}  // namespace mongo
