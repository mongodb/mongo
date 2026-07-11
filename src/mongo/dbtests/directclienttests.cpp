// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace DirectClientTests {

const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

class Base {
public:
    virtual ~Base() = default;
    virtual void run() = 0;
};

class InsertMany : public Base {
public:
    void run() override {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        std::vector<BSONObj> objs;
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

class BadNSCmd : public Base {
public:
    void run() override {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        BSONObj result;
        BSONObj cmdObj = BSON("count" << "");
        ASSERT(!client.runCommand(
            DatabaseName::createDatabaseName_forTest(boost::none, ""), cmdObj, result))
            << result;
        ASSERT_EQ(getStatusFromCommandResult(result), ErrorCodes::InvalidNamespace);
    }
};

class BadNSQuery : public Base {
public:
    void run() override {
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

class BadNSGetMore : public Base {
public:
    void run() override {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        ASSERT_THROWS_CODE(
            client.getMore(NamespaceString::createNamespaceString_forTest(""), 1)->nextSafe(),
            AssertionException,
            ErrorCodes::InvalidNamespace);
    }
};

class BadNSInsert : public Base {
public:
    void run() override {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        auto response = client.insertAcknowledged(
            NamespaceString::createNamespaceString_forTest(""), {BSONObj()});
        ASSERT_EQ(ErrorCodes::InvalidNamespace, getStatusFromCommandResult(response));
    }
};

class BadNSUpdate : public Base {
public:
    void run() override {
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

class BadNSRemove : public Base {
public:
    void run() override {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient client(&opCtx);

        auto response = client.removeAcknowledged(
            NamespaceString::createNamespaceString_forTest(""), BSONObj{} /*filter*/);
        ASSERT_EQ(ErrorCodes::InvalidNamespace, getStatusFromCommandResult(response));
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("directclient") {}
    void setupTests() override {
        add<InsertMany>();
        add<BadNSCmd>();
        add<BadNSQuery>();
        add<BadNSGetMore>();
        add<BadNSInsert>();
        add<BadNSUpdate>();
        add<BadNSRemove>();
    }
};

unittest::OldStyleSuiteInitializer<All> myall;

}  // namespace DirectClientTests
}  // namespace mongo
