/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_knobs/with_on_update_util.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {
namespace {

using Storage = WithOnUpdateHook<synchronized_value<int>>;

// Minimal ServerParameter whose storage is a WithOnUpdateHook<synchronized_value<int>>.
// Registered/deregistered dynamically so the test IDL is not touched.
class DynamicIntHookParam : public ServerParameter {
public:
    using ServerParameter::ServerParameter;
    using OnUpdateFn = WithOnUpdateHook<synchronized_value<int>>::OnUpdateFn;

    void append(OperationContext*,
                BSONObjBuilder* b,
                std::string_view name,
                const boost::optional<TenantId>&) override {
        *b << name << _data.get();
    }

    Status setFromString(std::string_view str, const boost::optional<TenantId>&) override {
        // _data = value triggers WithOnUpdateHook::operator=, which uasserts on hook error.
        _data = std::stoi(std::string(str));
        return Status::OK();
    }

    WithOnUpdateHook<synchronized_value<int>> _data;
};

class WithOnUpdateHookTest : public ServiceContextMongoDTest {
public:
    static constexpr auto kParamName = "dynamicIntHookParam";

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtxHolder = makeOperationContext();

        auto param = std::make_unique<DynamicIntHookParam>(kParamName,
                                                           ServerParameterType::kStartupAndRuntime);
        _param = param.get();
        ServerParameterSet::getNodeParameterSet()->add(std::move(param));
    }

    void tearDown() override {
        ServerParameterSet::getNodeParameterSet()->remove(kParamName);
        _param = nullptr;
        ServiceContextMongoDTest::tearDown();
    }

    void setHook(DynamicIntHookParam::OnUpdateFn fn) {
        _param->_data.setOnUpdate(std::move(fn));
    }

    int getValue() const {
        return _param->_data.get();
    }

protected:
    OperationContext* opCtx() {
        return _opCtxHolder.get();
    }

private:
    DynamicIntHookParam* _param = nullptr;
    ServiceContext::UniqueOperationContext _opCtxHolder;
};

TEST_F(WithOnUpdateHookTest, HookNotCalledWhenUnset) {
    Storage s;
    s = 42;
    ASSERT_EQ(s.get(), 42);
}

TEST_F(WithOnUpdateHookTest, HookCalledOnLvalueAssign) {
    Storage s;
    int seen = -1;
    s.setOnUpdate([&seen](const int& v) {
        seen = v;
        return Status::OK();
    });

    int val = 7;
    s = val;

    ASSERT_EQ(seen, 7);
    ASSERT_EQ(s.get(), 7);
}

TEST_F(WithOnUpdateHookTest, HookCalledOnRvalueAssign) {
    Storage s;
    int seen = -1;
    s.setOnUpdate([&seen](const int& v) {
        seen = v;
        return Status::OK();
    });

    s = 99;

    ASSERT_EQ(seen, 99);
    ASSERT_EQ(s.get(), 99);
}

TEST_F(WithOnUpdateHookTest, HookReceivesNewValue) {
    Storage s;
    std::vector<int> observed;
    s.setOnUpdate([&observed](const int& v) {
        observed.push_back(v);
        return Status::OK();
    });

    s = 1;
    s = 2;
    s = 3;

    ASSERT_EQ(observed.size(), 3u);
    ASSERT_EQ(observed[0], 1);
    ASSERT_EQ(observed[1], 2);
    ASSERT_EQ(observed[2], 3);
}

TEST_F(WithOnUpdateHookTest, ValueCommittedBeforeHookFires) {
    Storage s;
    int seenFromHook = -1;
    s.setOnUpdate([&s, &seenFromHook](const int& v) {
        seenFromHook = s.get();
        return Status::OK();
    });

    s = 55;

    ASSERT_EQ(seenFromHook, 55);
}

TEST_F(WithOnUpdateHookTest, HookErrorStatusThrows) {
    Storage s;
    s.setOnUpdate([](const int&) { return Status(ErrorCodes::InternalError, "hook error"); });
    ASSERT_THROWS_CODE(s = 10, DBException, ErrorCodes::InternalError);
}

TEST_F(WithOnUpdateHookTest, HookCanBeReplaced) {
    Storage s;
    int firstSeen = -1;
    int secondSeen = -1;

    s.setOnUpdate([&firstSeen](const int& v) {
        firstSeen = v;
        return Status::OK();
    });
    s = 5;
    ASSERT_EQ(firstSeen, 5);
    ASSERT_EQ(secondSeen, -1);

    s.setOnUpdate([&secondSeen](const int& v) {
        secondSeen = v;
        return Status::OK();
    });
    s = 6;
    ASSERT_EQ(firstSeen, 5);
    ASSERT_EQ(secondSeen, 6);
}

TEST_F(WithOnUpdateHookTest, HookErrorPropagatesThroughSetParameter) {
    setHook([](const int&) { return Status(ErrorCodes::InternalError, "hook error"); });

    // This is the same call the setParameter command makes (parameters.cpp).
    auto* sp = ServerParameterSet::getNodeParameterSet()->get(kParamName);
    ASSERT_THROWS_CODE(sp->set(BSON("" << 42).firstElement(), boost::none),
                       DBException,
                       ErrorCodes::InternalError);
}

TEST_F(WithOnUpdateHookTest, HookFiredViaRaiiController) {
    int seen = -1;
    setHook([&seen](const int& v) {
        seen = v;
        return Status::OK();
    });

    {
        unittest::ServerParameterGuard guard(kParamName, 42);
        ASSERT_EQ(seen, 42);
        ASSERT_EQ(getValue(), 42);
    }
    // Destructor restores the original value (0); hook fires again.
    ASSERT_EQ(seen, 0);
    ASSERT_EQ(getValue(), 0);
}

TEST_F(WithOnUpdateHookTest, ValueUpdatedViaDirectClient) {
    DBDirectClient client(opCtx());
    BSONObj result;
    client.runCommand(DatabaseName::kAdmin, BSON("setParameter" << 1 << kParamName << 7), result);

    ASSERT_EQ(result["ok"].Number(), 1.0);
    ASSERT_EQ(getValue(), 7);
}

TEST_F(WithOnUpdateHookTest, HookErrorPropagatesThroughDirectClient) {
    setHook([](const int&) { return Status(ErrorCodes::InternalError, "hook error"); });

    DBDirectClient client(opCtx());
    BSONObj result;
    client.runCommand(DatabaseName::kAdmin, BSON("setParameter" << 1 << kParamName << 42), result);

    ASSERT_EQ(result["ok"].Number(), 0.0);
    ASSERT_EQ(result["code"].Int(), ErrorCodes::InternalError);
    // Value is committed before the hook fires, matching scalar server param behaviour.
    ASSERT_EQ(getValue(), 42);
}

}  // namespace
}  // namespace mongo
