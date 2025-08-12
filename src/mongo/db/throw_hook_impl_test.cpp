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
        gThrowLoggingEnabled = true;
        gThrowLoggingExceptionName.clear();
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
    gThrowLoggingEnabled = false;
    throwSystemError();
    ASSERT(!didLog());
}

TEST_F(HookTest, HookEnabledSystemError) {
    gThrowLoggingEnabled = true;
    gThrowLoggingExceptionName.clear();
    throwSystemError();
    ASSERT(didLog());
}

TEST_F(HookTest, HookEnabledWithNameSystemError) {
    gThrowLoggingEnabled = true;
    gThrowLoggingExceptionName = dbErrorName();
    throwSystemError();
    ASSERT(didLog());
}

TEST_F(HookTest, HookDisabledDBError) {
    gThrowLoggingEnabled = false;
    throwDBError();
    ASSERT(!didLog());
}

TEST_F(HookTest, HookEnabledDBError) {
    gThrowLoggingEnabled = true;
    gThrowLoggingExceptionName.clear();
    throwDBError();
    ASSERT(!didLog());
}

TEST_F(HookTest, HookEnabledWithNameDBError) {
    gThrowLoggingEnabled = true;
    gThrowLoggingExceptionName = dbErrorName();
    throwDBError();
    ASSERT(didLog());
}

}  // namespace

}  // namespace mongo
