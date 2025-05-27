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
