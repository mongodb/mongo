// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/unittest/unittest.h"

#include <memory>

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
