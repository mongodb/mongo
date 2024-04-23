/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {

namespace {

#if __has_feature(address_sanitizer)

class ASANHandlesNoLockTest : public mongo::mozjs::MozJSImplScope::ASANHandlesNoLock {
public:
    ~ASANHandlesNoLockTest() = default;

    void assertExists(void* ptr, bool expected) const {
        if (expected) {
            ASSERT_TRUE(_handles.count(ptr));
        } else {
            ASSERT_FALSE(_handles.count(ptr));
        }
    }

    void assertExpectedUseCount(void* ptr, size_t expected) const {
        const auto iter = _handles.find(ptr);
        ASSERT_TRUE(iter != _handles.end());
        ASSERT_EQUALS(iter->second, expected);
    }
};

TEST(ASANHandlesTest, ASANHandlesTest) {

    ASANHandlesNoLockTest handles;
    auto p1 = std::make_unique<size_t>(0);

    handles.assertExists(p1.get(), false);

    handles.addPointer(p1.get());
    handles.assertExpectedUseCount(p1.get(), 1ULL);

    handles.addPointer(p1.get());
    handles.assertExpectedUseCount(p1.get(), 2ULL);

    handles.removePointer(p1.get());
    handles.assertExpectedUseCount(p1.get(), 1ULL);

    handles.addPointer(p1.get());
    handles.addPointer(p1.get());
    handles.assertExpectedUseCount(p1.get(), 3ULL);

    handles.removePointer(p1.get());
    handles.removePointer(p1.get());
    handles.removePointer(p1.get());
    handles.assertExists(p1.get(), false);
}
#endif

}  // namespace
}  // namespace mongo
