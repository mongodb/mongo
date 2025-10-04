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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
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
