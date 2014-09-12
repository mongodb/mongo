/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_coordinator_external_state_mock.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/sequence_util.h"

namespace mongo {
namespace repl {

    ReplicationCoordinatorExternalStateMock::ReplicationCoordinatorExternalStateMock()
        : _localRsConfigDocument(Status(ErrorCodes::NoMatchingDocument,
                                        "No local config document")),
         _connectionsClosed(false) {
    }

    ReplicationCoordinatorExternalStateMock::~ReplicationCoordinatorExternalStateMock() {}

    void ReplicationCoordinatorExternalStateMock::runSyncSourceFeedback() {}
    void ReplicationCoordinatorExternalStateMock::shutdown() {}
    void ReplicationCoordinatorExternalStateMock::forwardSlaveHandshake() {}
    void ReplicationCoordinatorExternalStateMock::forwardSlaveProgress() {}

    OID ReplicationCoordinatorExternalStateMock::ensureMe(OperationContext*) {
        return OID::gen();
    }

    bool ReplicationCoordinatorExternalStateMock::isSelf(const HostAndPort& host) {
        return sequenceContains(_selfHosts, host);
    }

    void ReplicationCoordinatorExternalStateMock::addSelf(const HostAndPort& host) {
        _selfHosts.push_back(host);
    }

    HostAndPort ReplicationCoordinatorExternalStateMock::getClientHostAndPort(
            const OperationContext* txn) {
        return _clientHostAndPort;
    }

    void ReplicationCoordinatorExternalStateMock::setClientHostAndPort(
            const HostAndPort& clientHostAndPort) {
        _clientHostAndPort = clientHostAndPort;
    }

    StatusWith<BSONObj> ReplicationCoordinatorExternalStateMock::loadLocalConfigDocument(
            OperationContext* txn) {
        return _localRsConfigDocument;
    }

    Status ReplicationCoordinatorExternalStateMock::storeLocalConfigDocument(
            OperationContext* txn,
            const BSONObj& config) {
        setLocalConfigDocument(StatusWith<BSONObj>(config));
        return Status::OK();
    }

    void ReplicationCoordinatorExternalStateMock::setLocalConfigDocument(
            const StatusWith<BSONObj>& localConfigDocument) {

        _localRsConfigDocument = localConfigDocument;
    }

    void ReplicationCoordinatorExternalStateMock::closeClientConnections() {
        _connectionsClosed = true;
    }

} // namespace repl
} // namespace mongo
