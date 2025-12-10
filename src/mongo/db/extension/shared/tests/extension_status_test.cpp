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

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo::extension {
namespace {

TEST(ExtensionStatusTest, extensionStatusOKTest) {
    // StatusHandle going out of scope should call destroy, but not destroy our singleton.
    auto* const statusOKSingleton = &ExtensionStatusOK::getInstance();
    {
        StatusHandle statusHandle(statusOKSingleton);
        ASSERT_EQUALS(statusHandle.getCode(), 0);
        ASSERT_EQUALS(statusHandle.getReason().size(), 0);
    }

    ASSERT_TRUE(statusOKSingleton == &ExtensionStatusOK::getInstance());

    ASSERT_EQUALS(ExtensionStatusOK::getInstance().getCode(), 0);
    ASSERT_EQUALS(ExtensionStatusOK::getInstance().getReason().size(), 0);
    ASSERT_EQUALS(ExtensionStatusOK::getInstanceCount(), 1);
}

TEST(ExtensionStatusTest, extensionStatusOKCloneTest) {
    // StatusHandle going out of scope should call destroy, but not destroy our singleton.
    auto* const statusOKSingleton = &ExtensionStatusOK::getInstance();
    {
        StatusHandle statusHandle(statusOKSingleton);
        ASSERT_EQUALS(statusHandle.getCode(), 0);
        ASSERT_EQUALS(statusHandle.getReason().size(), 0);

        auto clonedHandle = statusHandle.clone();
        ASSERT_EQUALS(statusHandle.getCode(), 0);
        ASSERT_EQUALS(statusHandle.getReason().size(), 0);
        ASSERT_EQUALS(ExtensionStatusOK::getInstanceCount(), 1);
    }

    ASSERT_TRUE(statusOKSingleton == &ExtensionStatusOK::getInstance());

    ASSERT_EQUALS(ExtensionStatusOK::getInstance().getCode(), 0);
    ASSERT_EQUALS(ExtensionStatusOK::getInstance().getReason().size(), 0);
    ASSERT_EQUALS(ExtensionStatusOK::getInstanceCount(), 1);
}

TEST(ExtensionStatusTest, extensionGenericStatusCloneTest) {
    StatusHandle statusHandle(new ExtensionGenericStatus(1, std::string("reason")));
    auto clonedHandle = statusHandle.clone();
    ASSERT_EQUALS(statusHandle.getCode(), clonedHandle.getCode());
    ASSERT_EQUALS(statusHandle.getReason(), clonedHandle.getReason());
}

TEST(ExtensionStatusTest, ExtensionStatusExceptionCloneTest) {
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { uasserted(11186305, "Failed with uassert in $noOpExtension parse."); }));

    auto clonedHandle = status.clone();
    ASSERT_EQUALS(status.getCode(), clonedHandle.getCode());
    ASSERT_EQUALS(status.getReason(), clonedHandle.getReason());

    auto exceptionPtr = ExtensionStatusException::extractException(*status.get());
    ASSERT(exceptionPtr);
    auto exceptionPtrFromClone = ExtensionStatusException::extractException(*clonedHandle.get());
    ASSERT(exceptionPtrFromClone);
    ASSERT_EQ(exceptionPtr, exceptionPtrFromClone);
}

// Test that a std::exception correctly returns MONGO_EXTENSION_STATUS_RUNTIME_ERROR when called via
// wrapCXXAndConvertExceptionToStatus.
TEST(ExtensionStatusTest, extensionStatusWrapCXXAndConvertExceptionToStatus_stdException) {
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { throw std::runtime_error("Runtime exception in $noOpExtension parse."); }));
    ASSERT_TRUE(status.getCode() == MONGO_EXTENSION_STATUS_RUNTIME_ERROR);
}

// Test that a std::exception can be rethrown when it crosses from a C++ context through the C API
// boundary and back to a C++ context.
TEST(
    ExtensionStatusTest,
    extensionStatusWrapCXXAndConvertExceptionToStatus_invokeCAndConvertStatusToException_rethrow_stdException) {
    ASSERT_THROWS(invokeCAndConvertStatusToException([&]() {
                      return wrapCXXAndConvertExceptionToStatus([&]() {
                          throw std::runtime_error("Runtime exception in $noOpExtension parse.");
                      });
                  }),
                  std::exception);
}

// Test that wrapCXXAndConvertExceptionToStatus correctly wraps a DBException (uassert) and returns
// the correct code when a call is made at the C API boundary.
TEST(ExtensionStatusTest, extensionStatusWrapCXXAndConvertExceptionToStatus_AssertionException) {
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { uasserted(10596408, "Failed with uassert in $noOpExtension parse."); }));
    ASSERT_TRUE(status.getCode() == 10596408);
}

// Test that a DBException (uassert) can be rethrown when it crosses from a C++ context through the
// C API boundary and back to a C++ context.
TEST(
    ExtensionStatusTest,
    extensionStatusWrapCXXAndConvertExceptionToStatus_invokeCAndConvertStatusToException_rethrow_AssertionException) {
    ASSERT_THROWS_CODE(invokeCAndConvertStatusToException([&]() {
                           return wrapCXXAndConvertExceptionToStatus([&]() {
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
TEST(
    ExtensionStatusTest,
    extensionStatusInvokeCAndConvertStatusToException_wrapCXXAndConvertExceptionToStatus_ExtensionDBException) {
    const std::string& kErrorString =
        "Failed with an error which was not an ExtensionStatusException.";
    auto extensionStatusPtr =
        std::make_unique<ExtensionStatusException>(nullptr, 10596412, kErrorString);
    const auto* extensionStatusOriginalPtr = extensionStatusPtr.get();

    StatusHandle propagatedStatus(wrapCXXAndConvertExceptionToStatus([&]() {
        invokeCAndConvertStatusToException([&]() { return extensionStatusPtr.release(); });
    }));

    // Verify that the MongoExtensionStatus* we propagated through
    // wrapCXXAndConvertExceptionToStatus(invokeCAndConvertStatusToException(..)) is the same one we
    // originally allocated.
    ASSERT_TRUE(propagatedStatus.get() == extensionStatusOriginalPtr);
}

TEST(ExtensionStatusTest, extensionStatusInvokeCAndConvertStatusToException_ExtensionDBException) {
    const std::string& kErrorString =
        "Failed with an error which was not an ExtensionStatusException.";
    ASSERT_THROWS_CODE_AND_WHAT(
        invokeCAndConvertStatusToException(
            [&]() { return new ExtensionStatusException(nullptr, 10596412, kErrorString); }),
        ExtensionDBException,
        10596412,
        kErrorString);
}

DEATH_TEST(ExtensionStatusTest, InvalidExtensionStatusVTableFailsGetCode, "10930105") {
    StatusHandle status(new ExtensionGenericStatus());
    auto vtable = status.vtable();
    vtable.get_code = nullptr;
    StatusHandle::assertVTableConstraintsHelper(vtable);
}

DEATH_TEST(ExtensionStatusTest, InvalidExtensionStatusVTableFailsGetReason, "10930106") {
    StatusHandle status(new ExtensionGenericStatus());
    auto vtable = status.vtable();
    vtable.get_reason = nullptr;
    StatusHandle::assertVTableConstraintsHelper(vtable);
}

DEATH_TEST(ExtensionStatusTest, InvalidExtensionStatusVTableFailsSetCode, "11186306") {
    StatusHandle status(new ExtensionGenericStatus());
    auto vtable = status.vtable();
    vtable.set_code = nullptr;
    StatusHandle::assertVTableConstraintsHelper(vtable);
}

DEATH_TEST(ExtensionStatusTest, InvalidExtensionStatusVTableFailsSetReason, "11186309") {
    StatusHandle status(new ExtensionGenericStatus());
    auto vtable = status.vtable();
    vtable.set_reason = nullptr;
    StatusHandle::assertVTableConstraintsHelper(vtable);
}

DEATH_TEST(ExtensionStatusTest, InvalidExtensionStatusVTableFailsClone, "11186310") {
    StatusHandle status(new ExtensionGenericStatus());
    auto vtable = status.vtable();
    vtable.clone = nullptr;
    StatusHandle::assertVTableConstraintsHelper(vtable);
}

DEATH_TEST(ExtensionStatusTest, ExtensionStatusOKSetReasonFails, "11186303") {
    StatusHandle status(&ExtensionStatusOK::getInstance());
    status.setReason(std::string(""));
}

TEST(ExtensionStatusTest, ExtensionStatusOKSetCodeNoOp) {
    StatusHandle status(&ExtensionStatusOK::getInstance());
    status.setCode(100);
    ASSERT_EQ(status.getCode(), 0);
}

DEATH_TEST(ExtensionStatusTest, ExtensionStatusExceptionSetReasonFails, "11186304") {
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { uasserted(11186311, "Failed with uassert in $noOpExtension parse."); }));
    status.setReason(std::string(""));
}

TEST(ExtensionStatusTest, ExtensionStatusExceptionSetCodeNoOp) {
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { uasserted(11186312, "Failed with uassert in $noOpExtension parse."); }));
    ASSERT_EQ(status.getCode(), 11186312);
    status.setCode(0);
    ASSERT_EQ(status.getCode(), 11186312);
}

}  // namespace
}  // namespace mongo::extension
