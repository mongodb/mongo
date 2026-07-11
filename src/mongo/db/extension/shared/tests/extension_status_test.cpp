// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/shared/extension_status.h"

#include "mongo/base/init.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo::extension {
namespace {

class TestObservabilityContext : public mongo::extension::ObservabilityContext {
public:
    ~TestObservabilityContext() override {}

    void extensionSuccess() const noexcept override {
        ++_sExtensionSuccessCounter;
    }

    void extensionError() const noexcept override {
        ++_sExtensionFailureCounter;
    }

    void hostSuccess() const noexcept override {
        ++_sHostSuccessCounter;
    }

    void hostError() const noexcept override {
        ++_sHostFailureCounter;
    }

    static size_t getExtensionSuccessCounter() {
        return _sExtensionSuccessCounter;
    }

    static size_t getExtensionFailureCounter() {
        return _sExtensionFailureCounter;
    }

    static size_t getHostSuccessCounter() {
        return _sHostSuccessCounter;
    }

    static size_t getHostFailureCounter() {
        return _sHostFailureCounter;
    }

    static void resetCounters() {
        _sExtensionSuccessCounter = 0;
        _sExtensionFailureCounter = 0;
        _sHostSuccessCounter = 0;
        _sHostFailureCounter = 0;
    }

private:
    static inline size_t _sExtensionSuccessCounter = 0;
    static inline size_t _sExtensionFailureCounter = 0;
    static inline size_t _sHostSuccessCounter = 0;
    static inline size_t _sHostFailureCounter = 0;
};

MONGO_INITIALIZER(InitializeTestObservabilityContext)(InitializerContext*) {
    mongo::extension::setGlobalObservabilityContext(std::make_unique<TestObservabilityContext>());
};

TEST(ExtensionStatusTest, extensionStatusOKTest) {
    // StatusHandle going out of scope should call destroy, but not destroy our singleton.
    auto* const statusOKSingleton = &ExtensionStatusOK::getInstance();
    {
        StatusHandle statusHandle(statusOKSingleton);
        ASSERT_EQUALS(statusHandle->getCode(), 0);
        ASSERT_EQUALS(statusHandle->getReason().size(), 0);
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
        ASSERT_EQUALS(statusHandle->getCode(), 0);
        ASSERT_EQUALS(statusHandle->getReason().size(), 0);

        auto clonedHandle = statusHandle->clone();
        ASSERT_EQUALS(statusHandle->getCode(), 0);
        ASSERT_EQUALS(statusHandle->getReason().size(), 0);
        ASSERT_EQUALS(ExtensionStatusOK::getInstanceCount(), 1);
    }

    ASSERT_TRUE(statusOKSingleton == &ExtensionStatusOK::getInstance());

    ASSERT_EQUALS(ExtensionStatusOK::getInstance().getCode(), 0);
    ASSERT_EQUALS(ExtensionStatusOK::getInstance().getReason().size(), 0);
    ASSERT_EQUALS(ExtensionStatusOK::getInstanceCount(), 1);
}

TEST(ExtensionStatusTest, extensionGenericStatusCloneTest) {
    StatusHandle statusHandle(new ExtensionGenericStatus(1, std::string("reason")));
    auto clonedHandle = statusHandle->clone();
    ASSERT_EQUALS(statusHandle->getCode(), clonedHandle->getCode());
    ASSERT_EQUALS(statusHandle->getReason(), clonedHandle->getReason());
}

TEST(ExtensionStatusTest, ExtensionStatusExceptionCloneTest) {
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { uasserted(11511001, "Failed with uassert in $noOpExtension parse."); }));

    auto clonedHandle = status->clone();
    ASSERT_EQUALS(status->getCode(), clonedHandle->getCode());
    ASSERT_EQUALS(status->getReason(), clonedHandle->getReason());

    auto exceptionPtr = ExtensionStatusException::extractException(*status.get());
    ASSERT(exceptionPtr);
    auto exceptionPtrFromClone = ExtensionStatusException::extractException(*clonedHandle.get());
    ASSERT(exceptionPtrFromClone);
    ASSERT_EQ(exceptionPtr, exceptionPtrFromClone);
}

/**
 * On Windows, calling std::current_exception() may create a copy of the original exception. On
 * Linux, exceptions are heap allocated, and the current exception pointer is treated like a
 * smartptr_handle, which does not invoke the copy constructor of the exception.
 *
 * Copying ExtensionStatusException incurs an additional hop across the API boundary, invoking
 * ExtensionStatusException::_extClone(). This additional hop across the boundary means that
 * the success counters will also be incremented due to the call to clone(), when propagating an
 * exception across the API boundary,
 *
 * It is important to note that the success counters are just an indicator of how many successful
 * calls were made across the API boundary. This discrepancy in metrics is not a detrimental side
 * effect of the copy constructor, and can be disregarded. In this helper function, we aim to
 * accommodate the discrepancy in test results between Windows & Linux platforms.
 *
 * NOTE: The discrepancy between the success counters is only present when std::current_exception()
 * is called. This can happen by some of our test helpers such as ASSERT_THROWS_CODE_*. While we
 * could adapt specific test cases depending on whether or not these test helpers are being used in
 * a test, it is simpler and more future-proof to assume the discrepancy is always present.
 *
 * When we call std::current_exception() (i.e from ASSERT_THROWS_CODE_*), this results in the
 * following:
 * 1) The Host calls clone() on the extension via invokeCAndConvertStatusToException. Since the
 * clone succeeds, this bumps the extension success counter by one.
 * 2) In order to service the clone() call, the extension wraps its business logic in a
 * wrapCXXAndConvertExceptionToStatus block. Because this unit test does not use dll, this results
 * in the host success count also incrementing by one.
 */
void assertExpectedMetricsOnError(uint32_t expectedExtensionFailures,
                                  uint32_t expectedHostFailures) {
    ASSERT_EQ(TestObservabilityContext::getExtensionFailureCounter(), expectedExtensionFailures);
    ASSERT_EQ(TestObservabilityContext::getHostFailureCounter(), expectedHostFailures);
#ifdef _WIN32
    ASSERT_LTE(TestObservabilityContext::getExtensionSuccessCounter(), 1u);
    ASSERT_LTE(TestObservabilityContext::getHostSuccessCounter(), 1u);
#else
    ASSERT_EQ(TestObservabilityContext::getExtensionSuccessCounter(), 0u);
    ASSERT_EQ(TestObservabilityContext::getHostSuccessCounter(), 0u);
#endif
}

void assertExpectedMetricsOnSuccess(uint32_t expectedExtensionSuccesses,
                                    uint32_t expectedHostSuccesses) {
    ASSERT_EQ(TestObservabilityContext::getExtensionSuccessCounter(), expectedExtensionSuccesses);
    ASSERT_EQ(TestObservabilityContext::getHostSuccessCounter(), expectedHostSuccesses);
    ASSERT_EQ(TestObservabilityContext::getExtensionFailureCounter(), 0u);
    ASSERT_EQ(TestObservabilityContext::getHostFailureCounter(), 0u);
}

// Test that a std::exception correctly returns MONGO_EXTENSION_STATUS_RUNTIME_ERROR when called via
// wrapCXXAndConvertExceptionToStatus.
TEST(ExtensionStatusTest, extensionStatusWrapCXXAndConvertExceptionToStatus_stdException) {
    TestObservabilityContext::resetCounters();
    const auto* obsCtx = mongo::extension::getGlobalObservabilityContext();
    const auto* cast = dynamic_cast<const TestObservabilityContext*>(obsCtx);
    ASSERT(cast != nullptr);
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { throw std::runtime_error("Runtime exception in $noOpExtension parse."); }));
    ASSERT_TRUE(status->getCode() == MONGO_EXTENSION_STATUS_RUNTIME_ERROR);
    assertExpectedMetricsOnError(0, 1);
}

// Test that a std::exception can be rethrown when it crosses from a C++ context through the C API
// boundary and back to a C++ context.
TEST(
    ExtensionStatusTest,
    extensionStatusWrapCXXAndConvertExceptionToStatus_invokeCAndConvertStatusToException_rethrow_stdException) {
    TestObservabilityContext::resetCounters();
    ASSERT_THROWS(invokeCAndConvertStatusToException([&]() {
                      return wrapCXXAndConvertExceptionToStatus([&]() {
                          throw std::runtime_error("Runtime exception in $noOpExtension parse.");
                      });
                  }),
                  std::exception);

    assertExpectedMetricsOnError(1, 1);
}

// Test that wrapCXXAndConvertExceptionToStatus correctly wraps a DBException (uassert) and returns
// the correct code when a call is made at the C API boundary.
TEST(ExtensionStatusTest, extensionStatusWrapCXXAndConvertExceptionToStatus_AssertionException) {
    TestObservabilityContext::resetCounters();
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { uasserted(10596408, "Failed with uassert in $noOpExtension parse."); }));
    ASSERT_TRUE(status->getCode() == 10596408);
    assertExpectedMetricsOnError(0, 1);
}

// Test that a DBException (uassert) can be rethrown when it crosses from a C++ context through the
// C API boundary and back to a C++ context.
TEST(
    ExtensionStatusTest,
    extensionStatusWrapCXXAndConvertExceptionToStatus_invokeCAndConvertStatusToException_rethrow_AssertionException) {
    TestObservabilityContext::resetCounters();
    ASSERT_THROWS_CODE(invokeCAndConvertStatusToException([&]() {
                           return wrapCXXAndConvertExceptionToStatus([&]() {
                               uasserted(10596409, "Failed with uassert in $noOpExtension parse.");
                           });
                       }),
                       AssertionException,
                       10596409);
    assertExpectedMetricsOnError(1, 1);
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
    TestObservabilityContext::resetCounters();

    StatusHandle propagatedStatus(wrapCXXAndConvertExceptionToStatus([&]() {
        invokeCAndConvertStatusToException([&]() { return extensionStatusPtr.release(); });
    }));

    // Verify that the MongoExtensionStatus* we propagated through
    // wrapCXXAndConvertExceptionToStatus(invokeCAndConvertStatusToException(..)) is the same one we
    // originally allocated.
    ASSERT_TRUE(propagatedStatus.get() == extensionStatusOriginalPtr);
    assertExpectedMetricsOnError(1, 1);
}

TEST(ExtensionStatusTest, extensionStatusInvokeCAndConvertStatusToException_ExtensionStatusOK) {
    TestObservabilityContext::resetCounters();
    invokeCAndConvertStatusToException([&]() { return &ExtensionStatusOK::getInstance(); });
    assertExpectedMetricsOnSuccess(1, 0);
}

TEST(ExtensionStatusTest,
     extensionStatusInvokeCAndConvertStatusToException_wrapCXXAndConvertExceptionToStatus_Success) {
    TestObservabilityContext::resetCounters();

    StatusHandle status(wrapCXXAndConvertExceptionToStatus([&]() { return true; }));
    ASSERT_TRUE(status->getCode() == MONGO_EXTENSION_STATUS_OK);
    assertExpectedMetricsOnSuccess(0, 1);
}

TEST(ExtensionStatusTest, extensionStatusInvokeCAndConvertStatusToException_ExtensionDBException) {
    const std::string& kErrorString =
        "Failed with an error which was not an ExtensionStatusException.";
    TestObservabilityContext::resetCounters();

    ASSERT_THROWS_CODE_AND_WHAT(
        invokeCAndConvertStatusToException(
            [&]() { return new ExtensionStatusException(nullptr, 10596412, kErrorString); }),
        ExtensionDBException,
        10596412,
        kErrorString);
    assertExpectedMetricsOnError(1, 0);
}

/*
 * Verify that an ExtensionDBException, propagates the underlying extension::ExtensionStatus
 * when it crosses the API boundary. An ExtensionDBException is typically thrown when we receive an
 * extension::ExtensionStatus that was not from the host's C++ context. In this case, there is
 * no underlying exception that can be rethrown. Instead, we throw a ExtensionDBException that wraps
 * the underlying MongoExtensionStatus.
 */
TEST(
    ExtensionStatusTest,
    extensionStatusInvokeCAndConvertStatusToException_wrapCXXAndConvertExceptionToStatus_MultipleRoundTrips) {
    TestObservabilityContext::resetCounters();
    // wrapCXX catches the rethrown exception, which is not an ExtensionDBException, so it
    // bumps the count again
    StatusHandle propagatedStatus(wrapCXXAndConvertExceptionToStatus([&]() {
        invokeCAndConvertStatusToException([&]() {
            // wrapCXX catches the rethrown exception, which is not an ExtensionDBException, so it
            // bumps the count again
            return wrapCXXAndConvertExceptionToStatus([&]() {
                invokeCAndConvertStatusToException([&]() {
                    // wrapCXX increases failure count to 1. allocates ExtensionStatusException
                    return wrapCXXAndConvertExceptionToStatus(
                        []() { uasserted(11569604, "Dummy error."); });
                });
            });
        });
    }));

    assertExpectedMetricsOnError(2, 3);
}

DEATH_TEST(ExtensionStatusTestDeathTest, InvalidExtensionStatusVTableFailsGetCode, "517") {
    auto vtable = ExtensionStatusOK::getVTable();
    vtable.get_code = nullptr;
    StatusAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(ExtensionStatusTestDeathTest, InvalidExtensionStatusVTableFailsGetReason, "517") {
    auto vtable = ExtensionStatusOK::getVTable();
    vtable.get_reason = nullptr;
    StatusAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(ExtensionStatusTestDeathTest, InvalidExtensionStatusVTableFailsSetCode, "517") {
    auto vtable = ExtensionStatusOK::getVTable();
    vtable.set_code = nullptr;
    StatusAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(ExtensionStatusTestDeathTest, InvalidExtensionStatusVTableFailsSetReason, "517") {
    auto vtable = ExtensionStatusOK::getVTable();
    vtable.set_reason = nullptr;
    StatusAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(ExtensionStatusTestDeathTest, InvalidExtensionStatusVTableFailsClone, "517") {
    auto vtable = ExtensionStatusOK::getVTable();
    vtable.clone = nullptr;
    StatusAPI::assertVTableConstraints(vtable);
}

DEATH_TEST(ExtensionStatusTestDeathTest, ExtensionStatusOKSetReasonFails, "11186303") {
    StatusHandle status(&ExtensionStatusOK::getInstance());
    status->setReason("");
}

TEST(ExtensionStatusTest, ExtensionStatusOKSetCodeNoOp) {
    StatusHandle status(&ExtensionStatusOK::getInstance());
    status->setCode(100);
    ASSERT_EQ(status->getCode(), 0);
}

DEATH_TEST(ExtensionStatusTestDeathTest, ExtensionStatusExceptionSetReasonFails, "11186304") {
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { uasserted(11186311, "Failed with uassert in $noOpExtension parse."); }));
    status->setReason("");
}

TEST(ExtensionStatusTest, ExtensionStatusExceptionSetCodeNoOp) {
    StatusHandle status(wrapCXXAndConvertExceptionToStatus(
        [&]() { uasserted(11186312, "Failed with uassert in $noOpExtension parse."); }));
    ASSERT_EQ(status->getCode(), 11186312);
    status->setCode(0);
    ASSERT_EQ(status->getCode(), 11186312);
}

}  // namespace
}  // namespace mongo::extension
