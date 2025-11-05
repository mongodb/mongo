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

#include "mongo/db/repl/clang_checked/checked_mutex.h"

#include "mongo/db/repl/clang_checked/mutex.h"
#include "mongo/db/repl/clang_checked/thread_safety_annotations.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/observable_mutex.h"

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
    auto f = Foo<stdx::mutex>(0);
    ASSERT_EQ(f.getI(), 0);
    f.incI();
    ASSERT_EQ(f.getI(), 1);
}

TEST(CheckedMutexTest, CheckedMutexShouldWorkWithObservableMutexStdxMutex) {
    auto f = Foo<ObservableMutex<mongo::stdx::mutex>>(0);
    ASSERT_EQ(f.getI(), 0);
    f.incI();
    ASSERT_EQ(f.getI(), 1);
}
}  // namespace
}  // namespace mongo
