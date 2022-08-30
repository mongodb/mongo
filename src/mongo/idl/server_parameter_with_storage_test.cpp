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

#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/idl/server_parameter_with_storage_test_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
AtomicWord<int> test::gStdIntPreallocated;
AtomicWord<int> test::gStdIntPreallocatedUpdateCount;
size_t test::count;

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
    T val = T();
    IDLServerParameterWithStorage<spt, T> param(name, val);
    using element_type = typename decltype(param)::element_type;

    // Check type coersion.
    for (const auto& v : valid) {
        element_type typedVal =
            uassertStatusOK(idl_server_parameter_detail::coerceFromString<element_type>(v));

        // setFromString() API.
        ASSERT_OK(param.setFromString(v, boost::none));
        ASSERT_EQ_OR_NAN(param.getValue(boost::none), typedVal);

        // set() API.
        ASSERT_OK(param.set(BSON("" << typedVal).firstElement(), boost::none));

        // append() API.
        BSONObjBuilder b;
        element_type exp;
        param.append(nullptr, &b, name.toString(), boost::none);
        ASSERT(b.obj().firstElement().coerce(&exp));
        ASSERT_EQ_OR_NAN(param.getValue(boost::none), exp);
    }
    for (const auto& v : invalid) {
        ASSERT_NOT_OK(param.setFromString(v, boost::none));
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
        ASSERT_OK(param.setFromString(valid[i], boost::none));
    }
    ASSERT_EQ(count, valid.size());

    // Check failed onUpdate does not block value being set.
    param.setOnUpdate([](const element_type&) { return Status(ErrorCodes::BadValue, "Go away"); });
    for (const auto& v : valid) {
        auto typedVal =
            uassertStatusOK(idl_server_parameter_detail::coerceFromString<element_type>(v));
        ASSERT_NOT_OK(param.setFromString(v, boost::none));
        ASSERT_EQ_OR_NAN(param.getValue(boost::none), typedVal);
    }

    // Clear onUpdate for next test.
    param.setOnUpdate(nullptr);
    ASSERT_OK(param.setFromString(valid[0], boost::none));

    // Check validation occurs and DOES block value being set.
    auto current = param.getValue(boost::none);
    param.addValidator([](const element_type&, const boost::optional<TenantId>&) {
        return Status(ErrorCodes::BadValue, "Go away");
    });
    for (const auto& v : valid) {
        ASSERT_NOT_OK(param.setFromString(v, boost::none));
        ASSERT_EQ_OR_NAN(current, param.getValue(boost::none));
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

    doStorageTestByAtomic<AtomicWord<bool>>("AtomicWord<bool>", boolVals, stringVals);
    doStorageTestByAtomic<AtomicWord<int>>("AtomicWord<int>", numberVals, stringVals);
    doStorageTestByAtomic<AtomicDouble>("AtomicDoubleI", numberVals, stringVals);
    doStorageTestByAtomic<AtomicDouble>("AtomicDoubleD", doubleVals, stringVals);
}

TEST(ServerParameterWithStorage, BoundsTest) {
    using idl_server_parameter_detail::GT;
    using idl_server_parameter_detail::LT;

    int val;
    IDLServerParameterWithStorage<SPT::kStartupOnly, int> param("BoundsTest", val);

    param.addBound<GT>(10);
    auto status = param.setFromString("5", boost::none);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.reason(), "Invalid value for parameter BoundsTest: 5 is not greater than 10");
    ASSERT_OK(param.setFromString("15", boost::none));

    param.addBound<LT>(20);
    ASSERT_OK(param.setValue(15, boost::none));
    status = param.setValue(25, boost::none);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.reason(), "Invalid value for parameter BoundsTest: 25 is not less than 20");
}

ServerParameter* getNodeServerParameter(const std::string& name) {
    return ServerParameterSet::getNodeParameterSet()->get(name);
}

ServerParameter* getClusterServerParameter(const std::string& name) {
    return ServerParameterSet::getClusterParameterSet()->get(name);
}

TEST(IDLServerParameterWithStorage, stdIntDeclared) {
    // 42 is set by "default" attribute in the IDL file.
    ASSERT_EQ(test::gStdIntDeclared.load(), 42);

    auto* stdIntDeclared = getNodeServerParameter("stdIntDeclared");
    ASSERT_OK(stdIntDeclared->setFromString("999", boost::none));
    ASSERT_EQ(test::gStdIntDeclared.load(), 999);
    ASSERT_NOT_OK(stdIntDeclared->setFromString("1000", boost::none));
    ASSERT_NOT_OK(stdIntDeclared->setFromString("-1", boost::none));
    ASSERT_NOT_OK(stdIntDeclared->setFromString("alpha", boost::none));

    // Reset to default.
    ASSERT_OK(stdIntDeclared->reset(boost::none));
    ASSERT_EQ(test::gStdIntDeclared.load(), 42);
}

TEST(IDLServerParameterWithStorage, stdIntPreallocated) {
    // 11 is set by "default" attribute in the IDL file.
    ASSERT_EQ(test::gStdIntPreallocated.load(), 11);
    // The Default set counts as an update.
    ASSERT_EQ(test::gStdIntPreallocatedUpdateCount.load(), 1);

    auto* stdIntPreallocated = getNodeServerParameter("stdIntPreallocated");
    ASSERT_OK(stdIntPreallocated->setFromString("41", boost::none));
    ASSERT_EQ(test::gStdIntPreallocated.load(), 41);
    ASSERT_EQ(test::gStdIntPreallocatedUpdateCount.load(), 2);

    ASSERT_NOT_OK(stdIntPreallocated->setFromString("42", boost::none));
    ASSERT_NOT_OK(stdIntPreallocated->setFromString("-1", boost::none));
    ASSERT_NOT_OK(stdIntPreallocated->setFromString("alpha", boost::none));
    ASSERT_EQ(test::gStdIntPreallocatedUpdateCount.load(), 2);

    // Reset to default.
    ASSERT_OK(stdIntPreallocated->reset(boost::none));
    ASSERT_EQ(test::gStdIntPreallocated.load(), 11);
    ASSERT_EQ(test::gStdIntPreallocatedUpdateCount.load(), 3);
}

TEST(IDLServerParameterWithStorage, startupString) {
    auto* sp = getNodeServerParameter("startupString");
    ASSERT_EQ(sp->allowedToChangeAtStartup(), true);
    ASSERT_EQ(sp->allowedToChangeAtRuntime(), false);
    ASSERT_OK(sp->setFromString("New Value", boost::none));
    ASSERT_EQ(test::gStartupString, "New Value");

    // Reset to default.
    ASSERT_OK(sp->reset(boost::none));
    ASSERT_EQ(test::gStartupString, "");
}

TEST(IDLServerParameterWithStorage, runtimeBoostDouble) {
    auto* sp = getNodeServerParameter("runtimeBoostDouble");
    ASSERT_EQ(sp->allowedToChangeAtStartup(), false);
    ASSERT_EQ(sp->allowedToChangeAtRuntime(), true);
    ASSERT_OK(sp->setFromString("1.0", boost::none));
    ASSERT_EQ(test::gRuntimeBoostDouble.get(), 1.0);

    // Reset to default.
    ASSERT_OK(sp->reset(boost::none));
    ASSERT_EQ(test::gRuntimeBoostDouble.get(), 0.0);
}

TEST(IDLServerParameterWithStorage, startupStringRedacted) {
    auto* sp = getNodeServerParameter("startupStringRedacted");
    ASSERT_OK(sp->setFromString("Hello World", boost::none));
    ASSERT_EQ(test::gStartupStringRedacted, "Hello World");

    BSONObjBuilder b;
    sp->append(nullptr, &b, sp->name(), boost::none);
    auto obj = b.obj();
    ASSERT_EQ(obj.nFields(), 1);
    ASSERT_EQ(obj[sp->name()].String(), "###");

    // Reset to default.
    ASSERT_OK(sp->reset(boost::none));
    ASSERT_EQ(test::gStartupStringRedacted, "");
}

TEST(IDLServerParameterWithStorage, startupIntWithExpressions) {
    auto* sp = dynamic_cast<IDLServerParameterWithStorage<SPT::kStartupOnly, std::int32_t>*>(
        getNodeServerParameter("startupIntWithExpressions"));
    ASSERT_EQ(test::gStartupIntWithExpressions, test::kStartupIntWithExpressionsDefault);

    ASSERT_NOT_OK(sp->setValue(test::kStartupIntWithExpressionsMinimum - 1, boost::none));
    ASSERT_OK(sp->setValue(test::kStartupIntWithExpressionsMinimum, boost::none));
    ASSERT_EQ(test::gStartupIntWithExpressions, test::kStartupIntWithExpressionsMinimum);

    ASSERT_NOT_OK(sp->setValue(test::kStartupIntWithExpressionsMaximum + 1, boost::none));
    ASSERT_OK(sp->setValue(test::kStartupIntWithExpressionsMaximum, boost::none));
    ASSERT_EQ(test::gStartupIntWithExpressions, test::kStartupIntWithExpressionsMaximum);
}

TEST(IDLServerParameterWithStorage, exportedDefaults) {
    ASSERT_EQ(test::kStdIntPreallocatedDefault, 11);
    ASSERT_EQ(test::kStdIntDeclaredDefault, 42);
    ASSERT_EQ(test::kStartupIntWithExpressionsDefault, 100);
    ASSERT_EQ(test::kUgly_complicated_name_spDefault, true);
}

// Test that the RAIIServerParameterControllerForTest works correctly on IDL-generated types.
TEST(IDLServerParameterWithStorage, RAIIServerParameterController) {
    // Test int
    auto* stdIntDeclared = getNodeServerParameter("stdIntDeclared");
    ASSERT_OK(stdIntDeclared->setFromString("42", boost::none));
    ASSERT_EQ(test::gStdIntDeclared.load(), 42);
    {
        RAIIServerParameterControllerForTest controller("stdIntDeclared", 10);
        ASSERT_EQ(test::gStdIntDeclared.load(), 10);
    }
    ASSERT_EQ(test::gStdIntDeclared.load(), 42);

    // Test bool
    auto* uglyComplicated = getNodeServerParameter("ugly complicated-name.sp");
    ASSERT_OK(uglyComplicated->setFromString("false", boost::none));
    ASSERT_EQ(test::gUglyComplicatedNameSp, false);
    {
        RAIIServerParameterControllerForTest controller("ugly complicated-name.sp", true);
        ASSERT_EQ(test::gUglyComplicatedNameSp, true);
    }
    ASSERT_EQ(test::gUglyComplicatedNameSp, false);

    // Test string
    auto* startupString = getNodeServerParameter("startupString");
    const auto coolStartupString = "Cool startup string";
    ASSERT_OK(startupString->setFromString(coolStartupString, boost::none));
    ASSERT_EQ(test::gStartupString, coolStartupString);
    {
        const auto badStartupString = "Bad startup string";
        RAIIServerParameterControllerForTest controller("startupString", badStartupString);
        ASSERT_EQ(test::gStartupString, badStartupString);
    }
    ASSERT_EQ(test::gStartupString, coolStartupString);
}

/**
 * IDLServerParameterWithStorage<SPT::kClusterWide> unit test.
 */
TEST(IDLServerParameterWithStorage, CSPStorageTest) {
    // Retrieve the cluster IDLServerParameterWithStorage.
    auto* clusterParam =
        dynamic_cast<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                   test::ChangeStreamOptionsClusterParam>*>(
            getClusterServerParameter("changeStreamOptions"));

    // Check that current value is the default value.
    test::ChangeStreamOptionsClusterParam retrievedParam = clusterParam->getValue(boost::none);
    ASSERT_EQ(retrievedParam.getPreAndPostImages().getExpireAfterSeconds(), 30);
    ASSERT_EQ(retrievedParam.getTestStringField(), "");
    ASSERT_EQ(clusterParam->getClusterParameterTime(boost::none), LogicalTime::kUninitialized);

    // Set to new value and check that the updated value is seen on get.
    test::ChangeStreamOptionsClusterParam updatedParam;
    test::PreAndPostImagesStruct updatedPrePostImgs;
    ClusterServerParameter baseCSP;

    updatedPrePostImgs.setExpireAfterSeconds(40);
    LogicalTime updateTime = LogicalTime(Timestamp(Date_t::now()));
    baseCSP.setClusterParameterTime(updateTime);
    baseCSP.set_id("changeStreamOptions"_sd);

    updatedParam.setClusterServerParameter(baseCSP);
    updatedParam.setPreAndPostImages(updatedPrePostImgs);
    updatedParam.setTestStringField("testString");
    ASSERT_OK(clusterParam->ServerParameter::set(updatedParam.toBSON(), boost::none));

    retrievedParam = clusterParam->getValue(boost::none);
    ASSERT_EQ(retrievedParam.getPreAndPostImages().getExpireAfterSeconds(), 40);
    ASSERT_EQ(retrievedParam.getTestStringField(), "testString");
    ASSERT_EQ(retrievedParam.getClusterParameterTime(), updateTime);
    ASSERT_EQ(clusterParam->getClusterParameterTime(boost::none), updateTime);
    ASSERT_EQ(test::count, 1);

    // Append to BSONObj and verify that expected fields are present.
    BSONObjBuilder b;
    clusterParam->append(nullptr, &b, clusterParam->name(), boost::none);
    auto obj = b.obj();
    ASSERT_EQ(obj.nFields(), 4);
    ASSERT_EQ(obj["_id"_sd].String(), "changeStreamOptions");
    ASSERT_EQ(obj["preAndPostImages"_sd].Obj()["expireAfterSeconds"].Long(), 40);
    ASSERT_EQ(obj["testStringField"_sd].String(), "testString");
    ASSERT_EQ(obj["clusterParameterTime"_sd].timestamp(), updateTime.asTimestamp());

    // setFromString should fail for cluster server parameters.
    ASSERT_NOT_OK(clusterParam->setFromString("", boost::none));

    // Reset the parameter and check that it now has its default value.
    ASSERT_OK(clusterParam->reset(boost::none));
    retrievedParam = clusterParam->getValue(boost::none);
    ASSERT_EQ(retrievedParam.getPreAndPostImages().getExpireAfterSeconds(), 30);
    ASSERT_EQ(retrievedParam.getTestStringField(), "");
    ASSERT_EQ(retrievedParam.getClusterParameterTime(), LogicalTime::kUninitialized);
    ASSERT_EQ(clusterParam->getClusterParameterTime(boost::none), LogicalTime::kUninitialized);
    ASSERT_EQ(test::count, 2);

    // Update the default value. The parameter should automatically reset to the new default value.
    test::ChangeStreamOptionsClusterParam newDefaultParam;
    test::PreAndPostImagesStruct newDefaultPrePostImgs;

    newDefaultPrePostImgs.setExpireAfterSeconds(35);
    newDefaultParam.setPreAndPostImages(newDefaultPrePostImgs);
    newDefaultParam.setTestStringField("default");
    ASSERT_OK(clusterParam->setDefault(newDefaultParam));
    retrievedParam = clusterParam->getValue(boost::none);
    ASSERT_EQ(retrievedParam.getPreAndPostImages().getExpireAfterSeconds(), 35);
    ASSERT_EQ(retrievedParam.getTestStringField(), "default");
    ASSERT_EQ(retrievedParam.getClusterParameterTime(), LogicalTime::kUninitialized);
    ASSERT_EQ(clusterParam->getClusterParameterTime(boost::none), LogicalTime::kUninitialized);
    ASSERT_EQ(test::count, 3);

    // Updating the default value a second time should have no effect.
    newDefaultPrePostImgs.setExpireAfterSeconds(45);
    newDefaultParam.setPreAndPostImages(newDefaultPrePostImgs);
    newDefaultParam.setTestStringField("newDefault");
    ASSERT_OK(clusterParam->setDefault(newDefaultParam));
    retrievedParam = clusterParam->getValue(boost::none);
    ASSERT_EQ(retrievedParam.getPreAndPostImages().getExpireAfterSeconds(), 35);
    ASSERT_EQ(retrievedParam.getTestStringField(), "default");
    ASSERT_EQ(test::count, 3);

    updatedPrePostImgs.setExpireAfterSeconds(-1);
    updatedParam.setPreAndPostImages(updatedPrePostImgs);
    updatedParam.setTestStringField("newTestString");
    updateTime = LogicalTime(Timestamp(Date_t::now()));
    ASSERT_NOT_OK(clusterParam->ServerParameter::validate(updatedParam.toBSON(), boost::none));
    ASSERT_NOT_OK(clusterParam->ServerParameter::set(updatedParam.toBSON(), boost::none));
    retrievedParam = clusterParam->getValue(boost::none);
    ASSERT_EQ(retrievedParam.getPreAndPostImages().getExpireAfterSeconds(), 35);
    ASSERT_EQ(retrievedParam.getTestStringField(), "default");
    ASSERT_EQ(clusterParam->getClusterParameterTime(boost::none), LogicalTime::kUninitialized);
    ASSERT_EQ(test::count, 3);
}

}  // namespace
}  // namespace mongo
