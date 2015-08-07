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
    ReplSetMetadata() = default;
    ReplSetMetadata(long long term,
                    repl::OpTime committedOpTime,
                    repl::OpTime visibleOpTime,
                    long long configVersion,
                    int currentPrimaryIndex);

    /**
     * format:
     * {
     *     term: 0,
     *     lastOpCommitted: {ts: Timestamp(0, 0), term: 0}
     *     lastOpVisible: {ts: Timestamp(0, 0), term: 0}
     *     configVersion: 0,
     *     primaryIndex: 0
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
     * Returns the index of the current primary from the perspective of the sender.
     */
    long long getPrimaryIndex() const {
        return _currentPrimaryIndex;
    }

    /**
     * Returns the current term from the perspective of the sender.
     */
    long long getTerm() const {
        return _currentTerm;
    }

private:
    repl::OpTime _lastOpCommitted =
        repl::OpTime(Timestamp(0, 0), repl::OpTime::kProtocolVersionV0Term);
    repl::OpTime _lastOpVisible =
        repl::OpTime(Timestamp(0, 0), repl::OpTime::kProtocolVersionV0Term);
    long long _currentTerm = -1;
    long long _configVersion = -1;
    int _currentPrimaryIndex = -1;
};

}  // namespace rpc
}  // namespace mongo
