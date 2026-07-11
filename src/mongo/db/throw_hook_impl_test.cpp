// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/throw_hook_options_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/throw_hook.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <exception>
#include <sstream>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {

class HookTest : public unittest::Test {
public:
    ~HookTest() override {
        gThrowLoggingEnabled.storeRelaxed(false);
        gThrowLoggingExceptionName->clear();
    }

    static void throwSystemError() {
        try {
            throw std::runtime_error("foo");
        } catch (...) {
        }
    }

    static void throwDBError() {
        try {
            throw TemporarilyUnavailableException(error_details::makeStatus(9891899, "foo"));
        } catch (...) {
        }
    }

    static auto dbErrorName() {
        return typeid(TemporarilyUnavailableException).name();
    }

    bool didLog() {
        return !_logs.getText().empty();
    }

private:
    unittest::LogCaptureGuard _logs;
};

TEST_F(HookTest, HookDisabledSystemError) {
    gThrowLoggingEnabled.storeRelaxed(false);
    throwSystemError();
    ASSERT(!didLog());
}

TEST_F(HookTest, HookEnabledSystemError) {
    gThrowLoggingEnabled.storeRelaxed(true);
    gThrowLoggingExceptionName->clear();
    throwSystemError();
    ASSERT(didLog());
}

TEST_F(HookTest, HookEnabledWithNameSystemError) {
    gThrowLoggingEnabled.storeRelaxed(true);
    gThrowLoggingExceptionName = dbErrorName();
    throwSystemError();
    ASSERT(didLog());
}

TEST_F(HookTest, HookDisabledDBError) {
    gThrowLoggingEnabled.storeRelaxed(false);
    throwDBError();
    ASSERT(!didLog());
}

TEST_F(HookTest, HookEnabledDBError) {
    gThrowLoggingEnabled.storeRelaxed(true);
    gThrowLoggingExceptionName->clear();
    throwDBError();
    ASSERT(!didLog());
}

TEST_F(HookTest, HookEnabledWithNameDBError) {
    gThrowLoggingEnabled.storeRelaxed(true);
    gThrowLoggingExceptionName = dbErrorName();
    throwDBError();
    ASSERT(didLog());
}

}  // namespace

}  // namespace mongo
