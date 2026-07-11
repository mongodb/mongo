// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/clang_checked/checked_mutex.h"

#include "mongo/db/repl/clang_checked/mutex.h"
#include "mongo/db/repl/clang_checked/thread_safety_annotations.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/observable_mutex.h"

#include <mutex>

namespace mongo {
namespace {

template <typename MutexT>
class Foo {
public:
    using mutex_type = MutexT;

    explicit Foo(int i) : _i(i) {}

    void incI() {
        clang_checked::lock_guard lk(_m);
        _i++;
    }

    int getI() {
        clang_checked::lock_guard lk(_m);
        return _i;
    }

private:
    mutable clang_checked::CheckedMutex<mutex_type> _m;

    int _i MONGO_LOCKING_GUARDED_BY(_m);
};

TEST(CheckedMutexTest, CheckedMutexShouldWorkWithStdxMutex) {
    auto f = Foo<std::mutex>(0);
    ASSERT_EQ(f.getI(), 0);
    f.incI();
    ASSERT_EQ(f.getI(), 1);
}

TEST(CheckedMutexTest, CheckedMutexShouldWorkWithObservableMutexStdxMutex) {
    auto f = Foo<ObservableMutex<std::mutex>>(0);
    ASSERT_EQ(f.getI(), 0);
    f.incI();
    ASSERT_EQ(f.getI(), 1);
}
}  // namespace
}  // namespace mongo
