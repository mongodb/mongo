/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/extension/shared/extension_status.h"

#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo::extension {
namespace {

TEST(ExtensionStatusTest, extensionStatusOKTest) {
    // HostStatusHandle going out of scope should call destroy, but not destroy our singleton.
    auto* const statusOKSingleton = &ExtensionStatusOK::getInstance();
    {
        HostStatusHandle statusHandle(statusOKSingleton);
        ASSERT_EQUALS(statusHandle.getCode(), 0);
        ASSERT_EQUALS(statusHandle.getReason().size(), 0);
    }

    ASSERT_TRUE(statusOKSingleton == &ExtensionStatusOK::getInstance());

    ASSERT_EQUALS(ExtensionStatusOK::getInstance().getCode(), 0);
    ASSERT_EQUALS(ExtensionStatusOK::getInstance().getReason().size(), 0);
    ASSERT_EQUALS(ExtensionStatusOK::getInstanceCount(), 1);
}

// Test that a std::exception correctly returns MONGO_EXTENSION_STATUS_RUNTIME_ERROR when called via
// enterCXX.
TEST(ExtensionStatusTest, extensionStatusEnterCXX_stdException) {
    HostStatusHandle status(enterCXX(
        [&]() { throw std::runtime_error("Runtime exception in $noOpExtension parse."); }));
    ASSERT_TRUE(status.getCode() == MONGO_EXTENSION_STATUS_RUNTIME_ERROR);
}

// Test that a std::exception can be rethrown when it crosses from a C++ context through the C API
// boundary and back to a C++ context.
TEST(ExtensionStatusTest, extensionStatusEnterCXX_enterC_rethrow_stdException) {
    ASSERT_THROWS(enterC([&]() {
                      return enterCXX([&]() {
                          throw std::runtime_error("Runtime exception in $noOpExtension parse.");
                      });
                  }),
                  std::exception);
}

// Test that enterCXX correctly wraps a DBException (uassert) and returns the correct code when a
// call is made at the C API boundary.
TEST(ExtensionStatusTest, extensionStatusEnterCXX_AssertionException) {
    HostStatusHandle status(
        enterCXX([&]() { uasserted(10596408, "Failed with uassert in $noOpExtension parse."); }));
    ASSERT_TRUE(status.getCode() == 10596408);
}

// Test that a DBException (uassert) can be rethrown when it crosses from a C++ context through the
// C API boundary and back to a C++ context.
TEST(ExtensionStatusTest, extensionStatusEnterCXX_enterC_rethrow_AssertionException) {
    ASSERT_THROWS_CODE(enterC([&]() {
                           return enterCXX([&]() {
                               uasserted(10596409, "Failed with uassert in $noOpExtension parse.");
                           });
                       }),
                       AssertionException,
                       10596409);
}

/*
 * Verify that an ExtensionDBException, propagates the underlying extension::ExtensionStatus
 * when it crosses the API boundary. An ExtensionDBException is typically thrown when we receive an
 * extension::ExtensionStatus that was not from the host's C++ context. In this case, there is
 * no underlying exception that can be rethrown. Instead, we throw a ExtensionDBException that wraps
 * the underlying MongoExtensinStatus.
 */
TEST(ExtensionStatusTest, extensionStatusEnterC_enterCXX_ExtensionDBException) {
    const std::string& kErrorString =
        "Failed with an error which was not an ExtensionStatusException.";
    auto extensionStatusPtr =
        std::make_unique<ExtensionStatusException>(nullptr, 10596412, kErrorString);
    const auto* extensionStatusOriginalPtr = extensionStatusPtr.get();

    HostStatusHandle propagatedStatus(
        enterCXX([&]() { enterC([&]() { return extensionStatusPtr.release(); }); }));

    // Verify that the MongoExtensionStatus* we propagated through enterCXX(enterC(..)) is the same
    // one we originally allocated.
    ASSERT_TRUE(propagatedStatus.get() == extensionStatusOriginalPtr);
}

TEST(ExtensionStatusTest, extensionStatusEnterC_ExtensionDBException) {
    const std::string& kErrorString =
        "Failed with an error which was not an ExtensionStatusException.";
    ASSERT_THROWS_CODE_AND_WHAT(enterC([&]() {
                                    return std::make_unique<ExtensionStatusException>(
                                               nullptr, 10596412, kErrorString)
                                        .release();
                                }),
                                ExtensionDBException,
                                10596412,
                                kErrorString);
}

}  // namespace
}  // namespace mongo::extension
