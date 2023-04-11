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

#include "mongo/platform/basic.h"

#include <iostream>

#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/timer.h"

namespace DirectClientTests {

using std::unique_ptr;
using std::vector;

const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

class InsertMany {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        vector<BSONObj> objs;
        objs.push_back(BSON("_id" << 1));
        objs.push_back(BSON("_id" << 1));
        objs.push_back(BSON("_id" << 2));

        client.dropCollection(nss);

        auto response = client.insertAcknowledged(nss, objs);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, getStatusFromWriteCommandReply(response));
        ASSERT_EQUALS((int)client.count(nss), 1);

        client.dropCollection(nss);

        response = client.insertAcknowledged(nss, objs, false /*ordered*/);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, getStatusFromWriteCommandReply(response));
        ASSERT_EQUALS((int)client.count(nss), 2);
    }
};

class BadNSCmd {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        BSONObj result;
        BSONObj cmdObj = BSON("count"
                              << "");
        ASSERT(!client.runCommand(
            DatabaseName::createDatabaseName_forTest(boost::none, ""), cmdObj, result))
            << result;
        ASSERT_EQ(getStatusFromCommandResult(result), ErrorCodes::InvalidNamespace);
    }
};

class BadNSQuery {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        FindCommandRequest findRequest{NamespaceString{}};
        findRequest.setLimit(1);
        ASSERT_THROWS_CODE(client.find(std::move(findRequest))->nextSafe(),
                           AssertionException,
                           ErrorCodes::InvalidNamespace);
    }
};

class BadNSGetMore {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        ASSERT_THROWS_CODE(
            client.getMore(NamespaceString::createNamespaceString_forTest(""), 1)->nextSafe(),
            AssertionException,
            ErrorCodes::InvalidNamespace);
    }
};

class BadNSInsert {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        auto response = client.insertAcknowledged(
            NamespaceString::createNamespaceString_forTest(""), {BSONObj()});
        ASSERT_EQ(ErrorCodes::InvalidNamespace, getStatusFromCommandResult(response));
    }
};

class BadNSUpdate {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        auto response =
            client.updateAcknowledged(NamespaceString::createNamespaceString_forTest(""),
                                      BSONObj{} /*filter*/,
                                      BSON("$set" << BSON("x" << 1)));
        ASSERT_EQ(ErrorCodes::InvalidNamespace, getStatusFromCommandResult(response));
    }
};

class BadNSRemove {
public:
    virtual void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        auto response = client.removeAcknowledged(
            NamespaceString::createNamespaceString_forTest(""), BSONObj{} /*filter*/);
        ASSERT_EQ(ErrorCodes::InvalidNamespace, getStatusFromCommandResult(response));
    }
};

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("directclient") {}
    void setupTests() {
        add<InsertMany>();
        add<BadNSCmd>();
        add<BadNSQuery>();
        add<BadNSGetMore>();
        add<BadNSInsert>();
        add<BadNSUpdate>();
        add<BadNSRemove>();
    }
};

OldStyleSuiteInitializer<All> myall;
}  // namespace DirectClientTests
