/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/tcmalloc_parameters_gen.h"
#include "mongo/util/tcmalloc_set_parameter.h"

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#include <tcmalloc/malloc_extension.h>
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
#include <gperftools/malloc_extension.h>
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE

namespace mongo {
namespace {
constexpr int testValInt = 753240;
constexpr bool testValBool = 0;
constexpr auto testValIntAsStr = "753240"_sd;
constexpr auto testValBoolAsStr = "0"_sd;

/**
 * This function runs a simple append test that builds up a BSONObj using tcmalloc server
 * parameters and verifies their values.
 *
 * @param param: The server parameter that we are getting the data from.
 * @param name: The name of the server parameter.
 * @param value: A size_t used to initialize param's value.
 * @param setTcmallocValue: The set helper function to use when setting the server parameter. It
 * should take in a StringData parameter name and a size_t value
 */
template <typename T, typename F>
void runAppendTest(T param, StringData name, size_t value, const F& setTcmallocValue) {
    ASSERT_DOES_NOT_THROW(setTcmallocValue(name, value));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "test", boost::none);
    ASSERT_TRUE(bob.hasField("test"));


    ASSERT_DOES_NOT_THROW(setTcmallocValue(name, value + 1));

    BSONObjBuilder subBob = bob.subobjStart("sub_doc");
    param.append(nullptr, &subBob, "sub_test", boost::none);
    ASSERT_TRUE(subBob.hasField("sub_test"));
    subBob.doneFast();

    ASSERT_TRUE(bob.hasField("sub_doc"));

    ASSERT_FALSE(bob.hasField(""));
    param.append(nullptr, &bob, "", boost::none);
    ASSERT_TRUE(bob.hasField(""));

    auto obj = bob.obj();
    for (const auto& e : obj) {
        if (e.fieldName() == "test"_sd) {
            ASSERT_EQ(e.numberLong(), value);
        } else if (e.fieldName() == "sub_doc"_sd) {
            for (const auto& subElem : e.embeddedObject()) {
                if (subElem.fieldName() == "sub_test"_sd) {
                    ASSERT_EQ(subElem.numberLong(), value + 1);
                }
            }
        } else if (e.fieldName() == ""_sd) {
            ASSERT_EQ(e.numberLong(), value + 1);
        }
    }
}

/**
 * This function runs a simple set test that builds up a BSONObj using tcmalloc server parameters
 * and verifies their values.
 *
 * @param param: The server parameter that we are setting the data on.
 * @param name: The name of the server parameter.
 * @param value: The value that is set on the parameter.
 * @param getTcmallocValue: The get helper function to use when getting the server parameter. It
 * should take in a StringData parameter name
 */
template <typename T, typename F>
void runSetTest(T param, StringData name, int value, const F& getTcmallocValue) {
    BSONElement emptyElem;
    ASSERT_EQ(ErrorCodes::TypeMismatch, param.set(emptyElem, boost::none));

    BSONObjBuilder bob;
    bob.appendNumber("a", -1);
    bob.appendNumber("b", value);
    BSONObj obj = bob.obj();

    BSONElement negVal = obj.getField("a");
    ASSERT_EQ(ErrorCodes::BadValue, param.set(negVal, boost::none));

    BSONElement val = obj.getField("b");
    ASSERT_OK(param.set(val, boost::none));

    size_t actualVal = getTcmallocValue(name);
    ASSERT_EQ(actualVal, value);
}

/**
 * This function runs a simple set from string test that builds up a BSONObj using tcmalloc server
 * parameters and verifies their values.
 *
 * @param param: The server parameter that we are setting the data on.
 * @param name: The name of the server parameter.
 * @param value: The StringData value that is set on the parameter.
 * @param getTcmallocValue: The get helper function to use when getting the server parameter. It
 * should take in a StringData parameter name
 */
template <typename T, typename F>
void runSetFromStringTest(T param, StringData name, StringData value, const F& getTcmallocValue) {
    int intVal;
    ASSERT_OK(NumberParser{}(value, &intVal));

    ASSERT_EQ(ErrorCodes::FailedToParse, param.setFromString("", boost::none));
    ASSERT_NOT_OK(param.setFromString("-1", boost::none));
    ASSERT_EQ(ErrorCodes::Overflow, param.setFromString(std::string(359, '9'), boost::none));
    ASSERT_OK(param.setFromString(value, boost::none));

    size_t actualVal = getTcmallocValue(name);
    ASSERT_EQ(actualVal, intVal);
}


TEST(MaxTotalThreadCacheBytesParam, AppendTest) {
    TCMallocMaxTotalThreadCacheBytesServerParameter param("tcmallocMaxTotalThreadCacheBytes"_sd,
                                                          ServerParameterType::kStartupAndRuntime);
    runAppendTest(param, kMaxTotalThreadCacheBytesPropertyName, testValInt, &setTcmallocProperty);
}

TEST(MaxTotalThreadCacheBytesParam, SetTest) {
    TCMallocMaxTotalThreadCacheBytesServerParameter param("tcmallocMaxTotalThreadCacheBytes"_sd,
                                                          ServerParameterType::kStartupAndRuntime);
    runSetTest(param, kMaxTotalThreadCacheBytesPropertyName, testValInt, &getTcmallocProperty);
}

TEST(MaxTotalThreadCacheBytesParam, SetFromStringTest) {
    TCMallocMaxTotalThreadCacheBytesServerParameter param("tcmallocMaxTotalThreadCacheBytes"_sd,
                                                          ServerParameterType::kStartupAndRuntime);
    runSetFromStringTest(
        param, kMaxTotalThreadCacheBytesPropertyName, testValIntAsStr, &getTcmallocProperty);
}

#if MONGO_CONFIG_TCMALLOC_GPERF
TEST(AggressiveMemoryDecommit, AppendTest) {
    TCMallocAggressiveMemoryDecommitServerParameter param("tcmallocAggressiveMemoryDecommit"_sd,
                                                          ServerParameterType::kStartupAndRuntime);
    runAppendTest(param, kAggressiveMemoryDecommitPropertyName, testValBool, &setTcmallocProperty);
}

TEST(AggressiveMemoryDecommit, SetTest) {
    TCMallocAggressiveMemoryDecommitServerParameter param("tcmallocAggressiveMemoryDecommit"_sd,
                                                          ServerParameterType::kStartupAndRuntime);
    runSetTest(param, kAggressiveMemoryDecommitPropertyName, testValBool, &getTcmallocProperty);
}

TEST(AggressiveMemoryDecommit, SetFromStringTest) {
    TCMallocAggressiveMemoryDecommitServerParameter param("tcmallocAggressiveMemoryDecommit"_sd,
                                                          ServerParameterType::kStartupAndRuntime);
    runSetFromStringTest(
        param, kAggressiveMemoryDecommitPropertyName, testValBoolAsStr, &getTcmallocProperty);
}
#endif  // MONGO_CONFIG_TCMALLOC_GPERF

TEST(ReleaseRate, AppendTest) {
    TCMallocReleaseRateServerParameter param("tcmallocReleaseRate"_sd,
                                             ServerParameterType::kStartupAndRuntime);
    runAppendTest(
        param, "", testValInt, [](StringData, size_t value) { setMemoryReleaseRate(value); });
}

TEST(ReleaseRate, SetFromStringTest) {
    TCMallocReleaseRateServerParameter param("tcmallocReleaseRate"_sd,
                                             ServerParameterType::kStartupAndRuntime);
    runSetFromStringTest(
        param, "", testValIntAsStr, [](StringData) { return getMemoryReleaseRate(); });
}
}  // namespace
}  // namespace mongo
