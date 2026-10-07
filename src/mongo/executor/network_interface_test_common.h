// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/baton.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/interruptible.h"

#include <cstdint>
#include <memory>
#include <ostream>

#include <boost/optional/optional.hpp>

/**
 * Test-only helpers shared by the NetworkInterface unit tests (network_interface_tl_test.cpp) and
 * integration tests (network_interface_integration_test.cpp): exact-counter assertions, a no-op
 * TaskExecutor::CallbackHandle, and the machinery for running each test case both with and without
 * a baton.
 */
namespace mongo::executor {

// For assertion error messages: "NetworkInterface::Counters{.sent=1, .canceled=0, ...}"
inline std::ostream& operator<<(std::ostream& out, const NetworkInterface::Counters& counters) {
#define PRINT(FIELD) "." #FIELD "=" << counters.FIELD
    return out << "NetworkInterface::Counters{" << PRINT(sent) << ", " << PRINT(canceled) << ", "
               << PRINT(timedOut) << ", " << PRINT(failed) << ", " << PRINT(failedRemotely)
               << ", " PRINT(succeeded) << "}";
#undef PRINT
}

/** Expected values for NetworkInterface::getCounters(). Fields left disengaged are not compared.*/
struct ExpectedCounters {
    boost::optional<uint64_t> sent;
    boost::optional<uint64_t> canceled;
    boost::optional<uint64_t> timedOut;
    boost::optional<uint64_t> failed;
    boost::optional<uint64_t> failedRemotely;
    boost::optional<uint64_t> succeeded;
};

// Compares the engaged fields of `expected` with the corresponding fields of `actual`.
inline bool operator==(const NetworkInterface::Counters& actual, const ExpectedCounters& expected) {
#define MATCH(FIELD) (!expected.FIELD.has_value() || actual.FIELD == *expected.FIELD)
    return MATCH(sent) && MATCH(canceled) && MATCH(timedOut) && MATCH(failed) &&
        MATCH(failedRemotely) && MATCH(succeeded);
#undef MATCH
}

// For assertion error messages: "ExpectedCounters{.sent=1, .canceled=0, ...}"
inline std::ostream& operator<<(std::ostream& out, const ExpectedCounters& expected) {
    bool first = true;
#define PRINT(FIELD)                              \
    if (expected.FIELD.has_value()) {             \
        if (first) {                              \
            first = false;                        \
        } else {                                  \
            out << ", ";                          \
        }                                         \
        out << "." #FIELD "=" << *expected.FIELD; \
    }
    out << "ExpectedCounters{";
    PRINT(sent)
    PRINT(canceled)
    PRINT(timedOut)
    PRINT(failed)
    PRINT(failedRemotely)
    PRINT(succeeded)
    out << "}";
    return out;
#undef PRINT
}

/** A mock class mimicking TaskExecutor::CallbackState, does nothing. */
class MockCallbackState final : public TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
    bool isCanceled() const override {
        return false;
    }
};

inline TaskExecutor::CallbackHandle makeCallbackHandle() {
    return TaskExecutor::CallbackHandle(std::make_shared<MockCallbackState>());
}

/**
 * Wraps a NetworkInterface test fixture so that every command carries a DefaultBaton and the
 * waiter pumps it through SemiFuture::get(opCtx). `Base` must declare
 * `virtual BatonHandle baton()` and `virtual Interruptible* interruptible()`, and route its
 * commands through them. The baton comes from an OperationContext on a separate ServiceContext.
 *
 * No tearDown() override: tests may call tearDown() while another thread is still blocked in
 * SemiFuture::get(interruptible()), so the OperationContext and baton live until destruction.
 */
template <typename Base>
class WithBatonFixture : public Base {
public:
    void setUp() override {
        Base::setUp();
        _serviceContext = ServiceContext::make();
        _client = _serviceContext->getService()->makeClient("BatonClient");
        _opCtx = _client->makeOperationContext();
        _baton = _opCtx->getBaton();
    }

    BatonHandle baton() override {
        return _baton;
    }

    Interruptible* interruptible() override {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueServiceContext _serviceContext;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    BatonHandle _baton;
};

/**
 * Defines a test body once and registers it in both `suite##WithBaton` and `suite##WithoutBaton`,
 * which must be fixtures deriving from `suite` (typically `WithBatonFixture<suite>` and an alias of
 * `suite`). `suite` must declare `void test##name();`.
 */
#define TEST_WITH_AND_WITHOUT_BATON_F(suite, name) \
    TEST_F(suite##WithBaton, name) {               \
        test##name();                              \
    }                                              \
    TEST_F(suite##WithoutBaton, name) {            \
        test##name();                              \
    }                                              \
    void suite::test##name()

}  // namespace mongo::executor
