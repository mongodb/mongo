/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/repl/repl_worker_pool_thread_count.h"

#include "mongo/db/repl/repl_writer_thread_pool_server_parameters_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/processinfo.h"

#include <algorithm>

namespace mongo {
namespace repl {
namespace {

class ReplWorkerPoolThreadCountParamsTest : public unittest::Test {
public:
    ReplWorkerPoolThreadCountParamsTest()
        : _threadCountCap(
              std::min(static_cast<int>(2 * ProcessInfo::getNumAvailableCores()), 256)) {}

    void setUp() override {
        ASSERT_GTE(_threadCountCap, 2);

        setMinThreadCount(0);
        setMaxThreadCount(16);
    }

protected:
    void setMinThreadCount(int newValue) {
        ASSERT_OK(validateUpdateReplWriterMinThreadCount(newValue, boost::none));
        replWriterMinThreadCount = newValue;
        ASSERT_OK(onUpdateReplWriterMinThreadCount(newValue));
        ASSERT_EQ(newValue, getMinThreadCountForReplWorkerPool());
    }

    void setMaxThreadCount(int newValue) {
        ASSERT_OK(validateUpdateReplWriterThreadCount(newValue, boost::none));
        replWriterThreadCount = newValue;
        ASSERT_OK(onUpdateReplWriterThreadCount(newValue));
        ASSERT_EQ(std::min(newValue, _threadCountCap), getThreadCountForReplWorkerPool());
    }

    int _threadCountCap;
};


TEST_F(ReplWorkerPoolThreadCountParamsTest, SetGetSuccess) {
    setMinThreadCount(0);
    setMaxThreadCount(1);

    setMaxThreadCount(2);
    setMinThreadCount(2);

    setMaxThreadCount(10);
    setMaxThreadCount(100);
    setMaxThreadCount(256);

    setMinThreadCount(_threadCountCap);
    setMinThreadCount(_threadCountCap - 1);
}

TEST_F(ReplWorkerPoolThreadCountParamsTest, SetFailure) {
    setMaxThreadCount(16);
    setMinThreadCount(2);

    // max < min
    ASSERT_NOT_OK(validateUpdateReplWriterThreadCount(1, boost::none));

    // min > max
    ASSERT_NOT_OK(validateUpdateReplWriterMinThreadCount(17, boost::none));
    ASSERT_NOT_OK(validateUpdateReplWriterMinThreadCount(_threadCountCap + 1, boost::none));

    // max out of bound
    ASSERT_NOT_OK(validateUpdateReplWriterThreadCount(-1, boost::none));
    ASSERT_NOT_OK(validateUpdateReplWriterThreadCount(0, boost::none));
    ASSERT_NOT_OK(validateUpdateReplWriterThreadCount(257, boost::none));
    ASSERT_NOT_OK(validateUpdateReplWriterThreadCount(1000, boost::none));

    // min out of bound
    ASSERT_NOT_OK(validateUpdateReplWriterMinThreadCount(-1, boost::none));
    ASSERT_NOT_OK(validateUpdateReplWriterMinThreadCount(257, boost::none));
    ASSERT_NOT_OK(validateUpdateReplWriterMinThreadCount(1000, boost::none));
}

TEST_F(ReplWorkerPoolThreadCountParamsTest, SetConcurrentSuccess) {
    const int newMinValue = 1;
    const int newMaxValue = _threadCountCap;

    // Start setting the min (this takes the lock)
    ASSERT_OK(validateUpdateReplWriterMinThreadCount(newMinValue, boost::none));

    stdx::thread setMaxThread([&] {
        // Set the max

        // This should block until the min has finished being set and the lock has been released.
        // This will take the lock.
        ASSERT_OK(validateUpdateReplWriterThreadCount(newMaxValue, boost::none));
        // Now the min should have finished being set.
        ASSERT_EQ(newMinValue, getMinThreadCountForReplWorkerPool());

        // Finish setting the max
        replWriterThreadCount = newMaxValue;
        // This will release the lock
        ASSERT_OK(onUpdateReplWriterThreadCount(newMaxValue));
        ASSERT_EQ(newMaxValue, getThreadCountForReplWorkerPool());
    });

    // Finish setting the min
    replWriterMinThreadCount = newMinValue;
    // This will release the lock
    ASSERT_OK(onUpdateReplWriterMinThreadCount(newMinValue));
    ASSERT_EQ(newMinValue, getMinThreadCountForReplWorkerPool());

    // Now setting the max can go through
    setMaxThread.join();
}

TEST_F(ReplWorkerPoolThreadCountParamsTest, SetConcurrentFailure) {
    const int newMinValue = 3;
    const int newMaxValue = 2;

    // Start setting the max (this takes the lock)
    ASSERT_OK(validateUpdateReplWriterThreadCount(newMaxValue, boost::none));

    stdx::thread setMinThread([&] {
        // Try setting the min

        // This should block until the max has finished being set and the lock has been released.
        // This will take the lock and immediately release it since the validation should fail.
        // This should fail as min > max.
        ASSERT_NOT_OK(validateUpdateReplWriterMinThreadCount(newMinValue, boost::none));
        // Now the max should have been set.
        ASSERT_EQ(newMaxValue, getThreadCountForReplWorkerPool());
    });

    // Finish setting the max
    replWriterThreadCount = newMaxValue;
    // This will release the lock
    ASSERT_OK(onUpdateReplWriterThreadCount(newMaxValue));
    ASSERT_EQ(newMaxValue, getThreadCountForReplWorkerPool());

    // Now trying to set the min can go through
    setMinThread.join();
}

}  // namespace
}  // namespace repl
}  // namespace mongo
