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

#include "mongo/idl/server_parameter_with_storage.h"
#include "mongo/idl/server_parameter_with_storage_test_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
AtomicInt32 test::gStdIntPreallocated;
AtomicInt32 test::gStdIntPreallocatedUpdateCount;

namespace {

using SPT = ServerParameterType;

// Normally `NaN != NaN`, but for the sake of this test,
// we want to pretend that it does.
template <typename T, typename U>
void ASSERT_EQ_OR_NAN(const T& a, const U& b) {
    if (a == a) {
        ASSERT_EQ(a, b);
    } else {
        // a is NaN, ergo, b must be NaN or they do not equal.
        ASSERT_FALSE(b == b);
    }
}

template <typename T, ServerParameterType spt>
void doStorageTest(StringData name,
                   const std::vector<std::string>& valid,
                   const std::vector<std::string>& invalid) {
    T val;
    IDLServerParameterWithStorage<T> param(name, val, spt);
    using element_type = typename decltype(param)::element_type;

    // Check type coersion.
    for (const auto& v : valid) {
        element_type typedVal =
            uassertStatusOK(idl_server_parameter_detail::coerceFromString<element_type>(v));

        // setFromString() API.
        ASSERT_OK(param.setFromString(v));
        ASSERT_EQ_OR_NAN(param.getValue(), typedVal);

        // set() API.
        ASSERT_OK(param.set(BSON("" << typedVal).firstElement()));

        // append() API.
        BSONObjBuilder b;
        element_type exp;
        param.append(nullptr, b, name.toString());
        ASSERT(b.obj().firstElement().coerce(&exp));
        ASSERT_EQ_OR_NAN(param.getValue(), exp);
    }
    for (const auto& v : invalid) {
        ASSERT_NOT_OK(param.setFromString(v));
        ASSERT_NOT_OK(idl_server_parameter_detail::coerceFromString<element_type>(v));
    }

    // Check onUpdate is invoked.
    size_t count = 0;
    param.setOnUpdate([&count](const element_type&) {
        ++count;
        return Status::OK();
    });
    for (size_t i = 0; i < valid.size(); ++i) {
        ASSERT_EQ(count, i);
        ASSERT_OK(param.setFromString(valid[i]));
    }
    ASSERT_EQ(count, valid.size());

    // Check failed onUpdate does not block value being set.
    param.setOnUpdate([](const element_type&) { return Status(ErrorCodes::BadValue, "Go away"); });
    for (const auto& v : valid) {
        auto typedVal =
            uassertStatusOK(idl_server_parameter_detail::coerceFromString<element_type>(v));
        ASSERT_NOT_OK(param.setFromString(v));
        ASSERT_EQ_OR_NAN(param.getValue(), typedVal);
    }

    // Clear onUpdate for next test.
    param.setOnUpdate(nullptr);
    ASSERT_OK(param.setFromString(valid[0]));

    // Check validation occurs and DOES block value being set.
    auto current = param.getValue();
    param.addValidator([](const element_type&) { return Status(ErrorCodes::BadValue, "Go away"); });
    for (const auto& v : valid) {
        ASSERT_NOT_OK(param.setFromString(v));
        ASSERT_EQ_OR_NAN(current, param.getValue());
    }
}

template <typename T>
void doStorageTestByType(const std::string& name,
                         const std::vector<std::string>& valid,
                         const std::vector<std::string>& invalid) {
    using SV = synchronized_value<T>;
    doStorageTest<T, SPT::kStartupOnly>("Startup" + name, valid, invalid);
    doStorageTest<SV, SPT::kStartupOnly>("BoostStartup" + name, valid, invalid);
    doStorageTest<SV, SPT::kRuntimeOnly>("Runtime" + name, valid, invalid);
    doStorageTest<SV, SPT::kStartupAndRuntime>("StartupAndRuntime" + name, valid, invalid);
}

template <typename T>
void doStorageTestByAtomic(const std::string& name,
                           const std::vector<std::string>& valid,
                           const std::vector<std::string>& invalid) {
    doStorageTest<T, SPT::kStartupOnly>("Startup" + name, valid, invalid);
    doStorageTest<T, SPT::kRuntimeOnly>("Runtime" + name, valid, invalid);
    doStorageTest<T, SPT::kStartupAndRuntime>("StartupAndRuntime" + name, valid, invalid);
}

TEST(ServerParameterWithStorage, StorageTest) {
    const std::vector<std::string> boolVals = {"true", "false", "1", "0"};
    const std::vector<std::string> numberVals = {"-2", "-1", "0", "1", "2", "3"};
    const std::vector<std::string> doubleVals = {"3.14", "2.71", "-1.1", "NaN", "INF", "-INF"};
    const std::vector<std::string> stringVals = {"purple", "moist"};

    doStorageTestByType<bool>("Bool", boolVals, stringVals);
    doStorageTestByType<std::int32_t>("Int32", numberVals, stringVals);
    doStorageTestByType<double>("DoubleI", numberVals, stringVals);
    doStorageTestByType<double>("DoubleD", doubleVals, stringVals);
    doStorageTestByType<std::string>("String", stringVals, {});

    doStorageTestByAtomic<AtomicBool>("AtomicBool", boolVals, stringVals);
    doStorageTestByAtomic<AtomicInt32>("AtomicInt32", numberVals, stringVals);
    doStorageTestByAtomic<AtomicDouble>("AtomicDoubleI", numberVals, stringVals);
    doStorageTestByAtomic<AtomicDouble>("AtomicDoubleD", doubleVals, stringVals);
}

TEST(ServerParameterWithStorage, BoundsTest) {
    using idl_server_parameter_detail::GT;
    using idl_server_parameter_detail::LT;

    int val;
    IDLServerParameterWithStorage<int> param("BoundsTest", val, SPT::kStartupOnly);

    param.addBound<GT>(10);
    auto status = param.setFromString("5");
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.reason(), "Invalid value for parameter BoundsTest: 5 is not greater than 10");
    ASSERT_OK(param.setFromString("15"));

    param.addBound<LT>(20);
    ASSERT_OK(param.setValue(15));
    status = param.setValue(25);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.reason(), "Invalid value for parameter BoundsTest: 25 is not less than 20");
}

ServerParameter* getServerParameter(const std::string& name) {
    const auto& spMap = ServerParameterSet::getGlobal()->getMap();
    const auto& spIt = spMap.find(name);
    ASSERT(spIt != spMap.end());

    auto* sp = spIt->second;
    ASSERT(sp);
    return sp;
}

TEST(IDLServerParameterWithStorage, stdIntDeclared) {
    // 42 is set by "default" attribute in the IDL file.
    ASSERT_EQ(test::gStdIntDeclared.load(), 42);

    auto* stdIntDeclared = getServerParameter("stdIntDeclared");
    ASSERT_OK(stdIntDeclared->setFromString("999"));
    ASSERT_EQ(test::gStdIntDeclared.load(), 999);
    ASSERT_NOT_OK(stdIntDeclared->setFromString("1000"));
    ASSERT_NOT_OK(stdIntDeclared->setFromString("-1"));
    ASSERT_NOT_OK(stdIntDeclared->setFromString("alpha"));
}

TEST(IDLServerParameterWithStorage, stdIntPreallocated) {
    // 11 is set by "default" attribute in the IDL file.
    ASSERT_EQ(test::gStdIntPreallocated.load(), 11);
    // The Default set counts as an update.
    ASSERT_EQ(test::gStdIntPreallocatedUpdateCount.load(), 1);

    auto* stdIntPreallocated = getServerParameter("stdIntPreallocated");
    ASSERT_OK(stdIntPreallocated->setFromString("41"));
    ASSERT_EQ(test::gStdIntPreallocated.load(), 41);
    ASSERT_EQ(test::gStdIntPreallocatedUpdateCount.load(), 2);

    ASSERT_NOT_OK(stdIntPreallocated->setFromString("42"));
    ASSERT_NOT_OK(stdIntPreallocated->setFromString("-1"));
    ASSERT_NOT_OK(stdIntPreallocated->setFromString("alpha"));
    ASSERT_EQ(test::gStdIntPreallocatedUpdateCount.load(), 2);
}

TEST(IDLServerParameterWithStorage, startupString) {
    auto* sp = getServerParameter("startupString");
    ASSERT_EQ(sp->allowedToChangeAtStartup(), true);
    ASSERT_EQ(sp->allowedToChangeAtRuntime(), false);
    ASSERT_OK(sp->setFromString("New Value"));
    ASSERT_EQ(test::gStartupString, "New Value");
}

TEST(IDLServerParameterWithStorage, runtimeBoostDouble) {
    auto* sp = getServerParameter("runtimeBoostDouble");
    ASSERT_EQ(sp->allowedToChangeAtStartup(), false);
    ASSERT_EQ(sp->allowedToChangeAtRuntime(), true);
    ASSERT_OK(sp->setFromString("1.0"));
    ASSERT_EQ(test::gRuntimeBoostDouble.get(), 1.0);
}

}  // namespace
}  // namespace mongo
