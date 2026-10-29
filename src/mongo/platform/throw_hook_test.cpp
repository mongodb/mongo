// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/throw_hook.h"

#include "mongo/unittest/unittest.h"

#include <exception>
#include <string>

namespace mongo {

namespace {

class ThrowTest : public unittest::Test {
public:
    ~ThrowTest() override {
        setThrowHook(_savedHook);
    }

    // Don't try asserting in here, exceptions will be swallowed.
    void throwHook(std::type_info* tinfo, void* inst) {
        trackedThrow = true;
        std::exception* ex = catchCast<std::exception>(tinfo, inst);
        if (ex) {
            castException = true;
            exMsg = ex->what();
        }
    }

    bool trackedThrow{false};
    bool castException{false};
    std::string exMsg;

    ThrowHook _savedHook{getThrowHook()};
};

TEST_F(ThrowTest, WithHook) {
    setThrowHook([this](std::type_info* tinfo, void* ex) { throwHook(tinfo, ex); });
    ASSERT(getThrowHook());
    try {
        throw std::runtime_error("foo");
    } catch (...) {
    }
    ASSERT(trackedThrow);
    ASSERT(castException);
    ASSERT_EQUALS(exMsg, "foo");
}

TEST_F(ThrowTest, NoHook) {
    setThrowHook(nullptr);
    ASSERT(!getThrowHook());
    try {
        throw std::runtime_error("foo");
    } catch (...) {
    }
    ASSERT(!trackedThrow);
    ASSERT(!castException);
    ASSERT(exMsg.empty());
}

}  // namespace

}  // namespace mongo
