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

class MigrationSecondaryThrottleOptions;
template <typename T>
class StatusWith;
class OperationContext;

/**
 * Returns the default write concern for migration cleanup on the donor shard and for cloning
 * documents on the destination shard.
 */
class ChunkMoveWriteConcernOptions {
public:
    /**
     * Based on the type of the server (standalone or replica set) and the requested secondary
     * throttle options returns what write concern should be used locally both for writing migrated
     * documents and for performing range deletions.
     *
     * Returns a non-OK status if the requested write concern cannot be satisfied for some reason.
     *
     * These are the rules for determining the local write concern to be used:
     *  - secondaryThrottle is not specified (kDefault) or it is on (kOn), but there is no custom
     *    write concern:
     *      - if replication is enabled and there are 2 or more nodes - w:2, j:false, timeout:60000
     *      - if replication is not enabled or less than 2 nodes - w:1, j:false, timeout:0
     *  - secondaryThrottle is off (kOff): w:1, j:false, timeout:0
     *  - secondaryThrottle is on (kOn) and there is custom write concern, use the custom write
     *    concern.
     */
    static StatusWith<WriteConcernOptions> getEffectiveWriteConcern(
        OperationContext* txn, const MigrationSecondaryThrottleOptions& options);
};

}  // namespace mongo
