/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/bson/oid.h"
#include "mongo/db/repl/optime.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;

namespace rpc {

extern const char kReplSetMetadataFieldName[];

/**
 * Represents the metadata information for $replData.
 */
class ReplSetMetadata {
public:
    /**
     * Default primary index. Also used to indicate in metadata that there is no
     * primary.
     */
    static const int kNoPrimary = -1;

    ReplSetMetadata() = default;
    ReplSetMetadata(long long term,
                    repl::OpTime committedOpTime,
                    repl::OpTime visibleOpTime,
                    long long configVersion,
                    OID replicaSetId,
                    int currentPrimaryIndex,
                    int currentSyncSourceIndex);

    /**
     * format:
     * {
     *     term: 0,
     *     lastOpCommitted: {ts: Timestamp(0, 0), term: 0},
     *     lastOpVisible: {ts: Timestamp(0, 0), term: 0},
     *     configVersion: 0,
     *     replicaSetId: ObjectId("..."), // Only present in certain versions and above.
     *     primaryIndex: 0,
     *     syncSourceIndex: 0
     * }
     */
    static StatusWith<ReplSetMetadata> readFromMetadata(const BSONObj& doc);
    Status writeToMetadata(BSONObjBuilder* builder) const;

    /**
     * Returns the OpTime of the most recent operation with which the client intereacted.
     */
    repl::OpTime getLastOpVisible() const {
        return _lastOpVisible;
    }

    /**
     * Returns the OpTime of the most recently committed op of which the sender was aware.
     */
    repl::OpTime getLastOpCommitted() const {
        return _lastOpCommitted;
    }

    /**
     * Returns the ReplSetConfig version number of the sender.
     */
    long long getConfigVersion() const {
        return _configVersion;
    }

    /**
     * Returns true if the sender has a replica set ID.
     */
    bool hasReplicaSetId() const {
        return _replicaSetId.isSet();
    }

    /**
     * Returns the replica set ID of the sender.
     */
    OID getReplicaSetId() const {
        return _replicaSetId;
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
     * Returns the current term from the perspective of the sender.
     */
    long long getTerm() const {
        return _currentTerm;
    }

private:
    repl::OpTime _lastOpCommitted;
    repl::OpTime _lastOpVisible;
    long long _currentTerm = -1;
    long long _configVersion = -1;
    OID _replicaSetId;
    int _currentPrimaryIndex = kNoPrimary;
    int _currentSyncSourceIndex = -1;
};

}  // namespace rpc
}  // namespace mongo
