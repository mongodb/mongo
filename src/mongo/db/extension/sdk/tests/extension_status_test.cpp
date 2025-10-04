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
#include "mongo/db/extension/sdk/extension_status.h"

#include "mongo/base/status.h"
#include "mongo/db/extension/sdk/byte_buf_utils.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo {
namespace {

TEST(ExtensionStatusTest, extensionStatusTest) {
    // Test default constructor of ExtensionStatus
    {
        mongo::extension::sdk::ExtensionStatus status;
        ASSERT_EQUALS(status.getCode(), 0);
        ASSERT_EQUALS(status.getReason().size(), 0);
    }

    // Test constructing an ExtensionStatus with an error code.
    {
        mongo::extension::sdk::ExtensionStatus status(20);
        ASSERT_EQUALS(status.getCode(), 20);
        ASSERT_EQUALS(status.getReason().size(), 0);
    }

    auto status = mongo::error_details::makeStatus(
        10596410, std::string("Assertion Exception in extension_status_test.cpp"));

    // Test constructing an ExtensionStatus with an exception.
    {
        mongo::extension::sdk::HostStatusHandle handle(nullptr);
        try {
            error_details::throwExceptionForStatus(status);
        } catch (const std::exception& exc) {
            handle = mongo::extension::sdk::HostStatusHandle(
                std::make_unique<mongo::extension::sdk::ExtensionStatus>(exc, status.code())
                    .release());
        }
        handle.assertValid();
        ASSERT_EQUALS(handle.getCode(), status.code());
        ASSERT_EQUALS(std::string(handle.getReason()), status.reason());
    }
    // Test setting an ExtensionStatus with an exception.
    {
        mongo::extension::sdk::ExtensionStatus extensionStatus;
        ASSERT_EQUALS(extensionStatus.getCode(), 0);
        try {
            error_details::throwExceptionForStatus(status);
        } catch (const std::exception& exc) {
            extensionStatus.setException(exc, -1);
        }
        ASSERT_EQUALS(extensionStatus.getCode(), -1);
        ASSERT_EQUALS(std::string(extensionStatus.getReason()), status.reason());
    }
}

TEST(ExtensionStatusTest, extensionStatusOKTest) {
    // HostStatusHandle going out of scope should call destroy, but not destroy our singleton.
    auto* const statusOKSingleton = &mongo::extension::sdk::ExtensionStatusOK::getInstance();
    {
        mongo::extension::sdk::HostStatusHandle statusHandle(statusOKSingleton);
        ASSERT_EQUALS(statusHandle.getCode(), 0);
        ASSERT_EQUALS(statusHandle.getReason().size(), 0);
    }

    ASSERT_TRUE(statusOKSingleton == &mongo::extension::sdk::ExtensionStatusOK::getInstance());

    ASSERT_EQUALS(mongo::extension::sdk::ExtensionStatusOK::getInstance().getCode(), 0);
    ASSERT_EQUALS(mongo::extension::sdk::ExtensionStatusOK::getInstance().getReason().size(), 0);
    ASSERT_EQUALS(mongo::extension::sdk::ExtensionStatusOK::getInstanceCount(), 1);
}

// Test that a std::exception correctly returns MONGO_EXTENSION_STATUS_RUNTIME_ERROR when called via
// enterCXX.
TEST(ExtensionStatusTest, extensionStatusEnterCXX_stdException) {
    mongo::extension::sdk::HostStatusHandle status(extension::sdk::enterCXX(
        [&]() { throw std::runtime_error("Runtime exception in $noOpExtension parse."); }));
    ASSERT_TRUE(status.getCode() == MONGO_EXTENSION_STATUS_RUNTIME_ERROR);
}

// Test that a std::exception can be rethrown when it crosses from a C++ context through the C API
// boundary and back to a C++ context.
TEST(ExtensionStatusTest, extensionStatusEnterCXX_enterC_rethrow_stdException) {
    ASSERT_THROWS(extension::sdk::enterC([&]() {
                      return extension::sdk::enterCXX([&]() {
                          throw std::runtime_error("Runtime exception in $noOpExtension parse.");
                      });
                  }),
                  std::exception);
}

// Test that enterCXX correctly wraps a DBException (uassert) and returns the correct code when a
// call is made at the C API boundary.
TEST(ExtensionStatusTest, extensionStatusEnterCXX_AssertionException) {
    mongo::extension::sdk::HostStatusHandle status(extension::sdk::enterCXX(
        [&]() { uasserted(10596408, "Failed with uassert in $noOpExtension parse."); }));
    ASSERT_TRUE(status.getCode() == 10596408);
}

// Test that a DBException (uassert) can be rethrown when it crosses from a C++ context through the
// C API boundary and back to a C++ context.
TEST(ExtensionStatusTest, extensionStatusEnterCXX_enterC_rethrow_AssertionException) {
    ASSERT_THROWS_CODE(extension::sdk::enterC([&]() {
                           return extension::sdk::enterCXX([&]() {
                               uasserted(10596409, "Failed with uassert in $noOpExtension parse.");
                           });
                       }),
                       AssertionException,
                       10596409);
}

/*
 * Verify that an ExtensionDBException, propagates the underlying extension::sdk::ExtensionStatus
 * when it crosses the API boundary. An ExtensionDBException is typically thrown when we receive an
 * extension::sdk::ExtensionStatus that was not from the host's C++ context. In this case, there is
 * no underlying exception that can be rethrown. Instead, we throw a ExtensionDBException that wraps
 * the underlying MongoExtensinStatus.
 */
TEST(ExtensionStatusTest, extensionStatusEnterC_enterCXX_ExtensionDBException) {
    const std::string& kErrorString =
        "Failed with an error which was not an ExtensionStatusException.";
    auto extensionStatusPtr =
        std::make_unique<extension::sdk::ExtensionStatus>(10596412, kErrorString);
    const auto* extensionStatusOriginalPtr = extensionStatusPtr.get();

    extension::sdk::HostStatusHandle propagatedStatus(extension::sdk::enterCXX(
        [&]() { extension::sdk::enterC([&]() { return extensionStatusPtr.release(); }); }));

    // Verify that the MongoExtensionStatus* we propagated through enterCXX(enterC(..)) is the same
    // one we originally allocated.
    ASSERT_TRUE(propagatedStatus.get() == extensionStatusOriginalPtr);
}

TEST(ExtensionStatusTest, extensionStatusEnterC_ExtensionDBException) {
    const std::string& kErrorString =
        "Failed with an error which was not an ExtensionStatusException.";
    ASSERT_THROWS_CODE_AND_WHAT(extension::sdk::enterC([&]() {
                                    return std::make_unique<extension::sdk::ExtensionStatus>(
                                               10596412, kErrorString)
                                        .release();
                                }),
                                extension::sdk::ExtensionDBException,
                                10596412,
                                kErrorString);
}

}  // namespace
}  // namespace mongo
