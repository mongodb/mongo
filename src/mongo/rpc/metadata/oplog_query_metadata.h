/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/oid.h"
#include "mongo/db/repl/optime.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;

namespace rpc {

extern const char kOplogQueryMetadataFieldName[];

/**
 * Represents the metadata information for $oplogQueryData.
 */
class OplogQueryMetadata {
public:
    /**
     * Default primary index. Also used to indicate in metadata that there is no
     * primary.
     */
    static const int kNoPrimary = -1;

    OplogQueryMetadata() = default;
    OplogQueryMetadata(repl::OpTimeAndWallTime lastOpCommitted,
                       repl::OpTime lastOpApplied,
                       int rbid,
                       int currentPrimaryIndex,
                       int currentSyncSourceIndex,
                       std::string currentSyncSourceHost);

    /**
     * format:
     * {
     *     lastOpCommitted: {ts: Timestamp(0, 0), term: 0},
     *     lastCommittedWall: ISODate("2018-07-25T19:21:22.449Z")
     *     lastOpApplied: {ts: Timestamp(0, 0), term: 0},
     *     rbid: 0
     *     primaryIndex: 0,
     *     syncSourceIndex: 0
     * }
     * requireWallTime is only false if FCV is less than 4.2 or the wall clock time is not read from
     * this particular OplogQueryMetadata instance.
     */
    static StatusWith<OplogQueryMetadata> readFromMetadata(const BSONObj& doc,
                                                           bool requireWallTime);
    Status writeToMetadata(BSONObjBuilder* builder) const;

    /**
     * Returns the OpTime of the most recently committed op of which the sender was aware.
     */
    repl::OpTimeAndWallTime getLastOpCommitted() const {
        return _lastOpCommitted;
    }

    /**
     * Returns the OpTime of the most recent operation to be applied by the sender.
     */
    repl::OpTime getLastOpApplied() const {
        return _lastOpApplied;
    }

    /**
     * Returns the index of the current primary from the perspective of the sender.
     * Returns kNoPrimary if there is no primary.
     */
    int getPrimaryIndex() const {
        return _currentPrimaryIndex;
    }

    /**
     * Returns the index of the sync source of the sender.
     * Returns -1 if it has no sync source.
     */
    int getSyncSourceIndex() const {
        return _currentSyncSourceIndex;
    }

    /**
     * Returns the host of the sync source of the sender.
     * Returns empty string if it has no sync source.
     */
    std::string getSyncSourceHost() const {
        return _currentSyncSourceHost;
    }

    /**
     * Returns the current rbid of the sender.
     */
    int getRBID() const {
        return _rbid;
    }

    /**
     * Returns a stringified version of the metadata.
     */
    std::string toString() const;

private:
    repl::OpTimeAndWallTime _lastOpCommitted;
    repl::OpTime _lastOpApplied;
    int _rbid = -1;
    int _currentPrimaryIndex = kNoPrimary;
    int _currentSyncSourceIndex = -1;
    std::string _currentSyncSourceHost;
};

}  // namespace rpc
}  // namespace mongo
