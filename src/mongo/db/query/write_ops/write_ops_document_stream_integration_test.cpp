// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {

TEST(WriteOpsDocSeq, InsertDocStreamWorks) {
    auto swConn = unittest::getFixtureConnectionString().connect("integration_test");
    uassertStatusOK(swConn.getStatus());
    auto conn = std::move(swConn.getValue());

    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test", "doc_seq");
    conn->dropCollection(ns);
    ASSERT_EQ(conn->count(ns), 0u);

    OpMsgRequest request;
    request.body = BSON("insert" << ns.coll() << "$db" << ns.db_forTest());
    request.sequences = {{"documents",
                          {
                              BSON("_id" << 1),
                              BSON("_id" << 2),
                              BSON("_id" << 3),
                              BSON("_id" << 4),
                              BSON("_id" << 5),
                          }}};

    const auto reply = conn->runCommand(std::move(request));
    ASSERT_EQ(int(reply->getProtocol()), int(rpc::Protocol::kOpMsg));
    auto body = reply->getCommandReply();
    ASSERT_OK(getStatusFromCommandResult(body));
    ASSERT_EQ(body["n"].Int(), 5);
    ASSERT_EQ(conn->count(ns), 5u);
}

}  // namespace mongo
