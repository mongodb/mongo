// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/platform/atomic.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {
namespace {

class UglyBSONFixture : public NetworkInterfaceIntegrationFixture {
    void setUp() override {
        startNet();
    }
};

TEST_F(UglyBSONFixture, DuplicateFields) {
    OpMsgBuilder::disableDupeFieldCheck_forTest.store(true);
    ON_BLOCK_EXIT([] { OpMsgBuilder::disableDupeFieldCheck_forTest.store(false); });

    assertCommandFailsOnServer(DatabaseName::kAdmin,
                               BSON("insert" << "test"
                                             << "documents" << BSONArray() << "documents"
                                             << BSONArray()),
                               ErrorCodes::IDLDuplicateField);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
