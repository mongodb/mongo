/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/temp_dir.h"

namespace mongo {
/**
 * This is a simple fixture for use with the OpMsgFuzzer.
 *
 * In essenence, this is equivalent to making a standalone mongod with a single client.
 */
class OpMsgFuzzerFixture {
public:
    OpMsgFuzzerFixture(bool skipGlobalInitializers = false);

    ~OpMsgFuzzerFixture();

    /**
     * Run a single operation as if it came from the network.
     */
    int testOneInput(const char* Data, size_t Size);

private:
    void _setAuthorizationManager();

    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(3, 1));

    // This member is responsible for both creating and deleting the base directory. Think of it as
    // a smart pointer to the directory.
    const unittest::TempDir _dir;

    ServiceContext* _serviceContext;
    ClientStrandPtr _clientStrand;
    transport::TransportLayerMock _transportLayer;
    transport::SessionHandle _session;
    AuthzManagerExternalStateMock* _externalState;
    AuthorizationManagerImpl* _authzManager;
};
}  // namespace mongo
