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
#include "mongo/db/client.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/snapshot_name.h"

namespace mongo {

class BSONObjBuilder;
class Client;
class OperationContext;

namespace repl {

class ReplClientInfo {
public:
    static const Client::Decoration<ReplClientInfo> forClient;

    void setLastOp(const OpTime& op) {
        _lastOp = op;
    }
    OpTime getLastOp() const {
        return _lastOp;
    }

    void setLastSnapshot(SnapshotName name) {
        _lastSnapshot = name;
    }
    SnapshotName getLastSnapshot() const {
        return _lastSnapshot;
    }

    // Only used for master/slave
    void setRemoteID(OID rid) {
        _remoteId = rid;
    }
    OID getRemoteID() const {
        return _remoteId;
    }

    /**
     * Use this to set the LastOp to the latest known OpTime in the oplog.
     * This is necessary when doing no-op writes, as we need to set the client's lastOp to a proper
     * value for write concern wait to work.
     */
    void setLastOpToSystemLastOpTime(OperationContext* txn);

private:
    static const long long kUninitializedTerm = -1;

    OpTime _lastOp = OpTime();
    SnapshotName _lastSnapshot = SnapshotName::min();
    OID _remoteId = OID();
};

}  // namespace repl
}  // namespace mongo
