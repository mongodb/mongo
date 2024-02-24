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

#include <boost/none.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/oplog_buffer_proxy.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class OplogBufferMock : public OplogBuffer {
    OplogBufferMock(const OplogBufferMock&) = delete;
    OplogBufferMock& operator=(const OplogBufferMock&) = delete;

public:
    OplogBufferMock() = default;
    virtual ~OplogBufferMock() = default;

    void startup(OperationContext*) override {
        startupCalled = true;
    }
    void shutdown(OperationContext*) override {
        shutdownCalled = true;
    }
    void push(OperationContext* opCtx,
              Batch::const_iterator begin,
              Batch::const_iterator end,
              boost::optional<std::size_t> bytes) override {
        for (auto i = begin; i != end; ++i) {
            values.push_back(*i);
        }
    }
    void waitForSpace(OperationContext*, std::size_t) override {
        waitForSpaceCalled = true;
    }
    bool isEmpty() const override {
        return values.empty();
    }
    std::size_t getMaxSize() const override {
        return maxSize;
    }
    std::size_t getSize() const override {
        std::size_t totalSize = 0;
        for (auto&& obj : values) {
            totalSize += obj.objsize();
        }
        return totalSize;
    }
    std::size_t getCount() const override {
        return values.size();
    }
    void clear(OperationContext*) override {
        values.clear();
    }
    bool tryPop(OperationContext* opCtx, Value* value) override {
        tryPopCalled = true;
        if (!peek(opCtx, value)) {
            return false;
        }
        values.pop_front();
        return true;
    }
    bool waitForDataFor(Milliseconds, Interruptible*) override {
        // Blocking not supported.
        waitForDataCalled = true;
        return !values.empty();
    }
    bool waitForDataUntil(Date_t, Interruptible*) override {
        // Blocking not supported.
        waitForDataUntilCalled = true;
        return !values.empty();
    }
    bool peek(OperationContext*, Value* value) override {
        peekCalled = true;
        if (values.empty()) {
            return false;
        }
        *value = values.front();
        return true;
    }
    /**
     * Returns boost::none because this function should never be called by the proxy.
     */
    boost::optional<Value> lastObjectPushed(OperationContext*) const override {
        lastObjectPushedCalled = true;
        return boost::none;
    }

    bool startupCalled = false;
    bool shutdownCalled = false;
    bool waitForSpaceCalled = false;
    bool waitForDataCalled = false;
    bool waitForDataUntilCalled = false;
    bool tryPopCalled = false;
    bool peekCalled = false;
    mutable bool lastObjectPushedCalled = false;
    std::deque<Value> values;
    std::size_t maxSize = 0U;
};

class OplogBufferProxyTest : public unittest::Test {
private:
    void setUp() override;
    void tearDown() override;

protected:
    OplogBufferMock* _mock = nullptr;
    std::unique_ptr<OplogBufferProxy> _proxy;
    OperationContext* _opCtx = nullptr;  // Not dereferenced.
};

void OplogBufferProxyTest::setUp() {
    auto mock = std::make_unique<OplogBufferMock>();
    _mock = mock.get();
    _proxy = std::make_unique<OplogBufferProxy>(std::move(mock));
}

void OplogBufferProxyTest::tearDown() {
    _proxy.reset();
    _mock = nullptr;
}

DEATH_TEST_REGEX_F(OplogBufferProxyTest,
                   NullTargetOplogBufferAtConstructionTriggersInvariant,
                   "Invariant failure.*_target") {
    OplogBufferProxy(nullptr);
}

TEST_F(OplogBufferProxyTest, GetTarget) {
    ASSERT_EQUALS(_mock, _proxy->getTarget());
}

TEST_F(OplogBufferProxyTest, Startup) {
    _proxy->startup(_opCtx);
    ASSERT_TRUE(_mock->startupCalled);
}

TEST_F(OplogBufferProxyTest, ShutdownResetsCachedValues) {
    OplogBuffer::Batch values = {BSON("x" << 1)};
    _proxy->push(_opCtx, values.cbegin(), values.cend(), boost::none);
    OplogBuffer::Value peekValue;
    ASSERT_TRUE(_proxy->peek(_opCtx, &peekValue));
    ASSERT_BSONOBJ_EQ(values[0], peekValue);

    ASSERT_NOT_EQUALS(boost::none, _proxy->lastObjectPushed(_opCtx));
    ASSERT_NOT_EQUALS(boost::none, _proxy->getLastPeeked_forTest());

    _proxy->shutdown(_opCtx);
    ASSERT_TRUE(_mock->shutdownCalled);

    ASSERT_EQUALS(boost::none, _proxy->lastObjectPushed(_opCtx));
    ASSERT_EQUALS(boost::none, _proxy->getLastPeeked_forTest());
}

TEST_F(OplogBufferProxyTest, WaitForSpace) {
    _proxy->waitForSpace(_opCtx, 100U);
    ASSERT_TRUE(_mock->waitForSpaceCalled);
}

TEST_F(OplogBufferProxyTest, MaxSize) {
    _mock->maxSize = 8888U;
    ASSERT_EQUALS(_mock->maxSize, _proxy->getMaxSize());
}

TEST_F(OplogBufferProxyTest, EmptySizeAndCount) {
    ASSERT_TRUE(_proxy->isEmpty());
    OplogBuffer::Batch values = {BSON("x" << 1), BSON("x" << 2)};
    _proxy->push(_opCtx, values.cbegin(), values.cend(), boost::none);
    ASSERT_FALSE(_proxy->isEmpty());
    ASSERT_EQUALS(values.size(), _mock->getCount());
    ASSERT_EQUALS(_mock->getCount(), _proxy->getCount());
    ASSERT_EQUALS(std::size_t(values[0].objsize() + values[1].objsize()), _mock->getSize());
    ASSERT_EQUALS(_mock->getSize(), _proxy->getSize());
}

TEST_F(OplogBufferProxyTest, ClearResetsCachedValues) {
    OplogBuffer::Batch values = {BSON("x" << 1), BSON("x" << 2)};
    _proxy->push(_opCtx, values.cbegin(), values.cend(), boost::none);
    ASSERT_FALSE(_mock->isEmpty());
    auto lastObjPushed = _proxy->lastObjectPushed(_opCtx);
    ASSERT_NOT_EQUALS(boost::none, lastObjPushed);
    ASSERT_BSONOBJ_EQ(values.back(), *lastObjPushed);
    ASSERT_FALSE(_mock->lastObjectPushedCalled);

    OplogBuffer::Value peekValue;
    ASSERT_TRUE(_proxy->peek(_opCtx, &peekValue));
    ASSERT_NOT_EQUALS(boost::none, _proxy->getLastPeeked_forTest());

    _proxy->clear(_opCtx);
    ASSERT_TRUE(_mock->isEmpty());
    ASSERT_EQUALS(boost::none, _proxy->lastObjectPushed(_opCtx));
    ASSERT_EQUALS(boost::none, _proxy->getLastPeeked_forTest());
}

void _testPushFunctionUpdatesCachedLastObjectPushed(
    OperationContext* opCtx,
    OplogBuffer* proxy,
    OplogBufferMock* mock,
    std::function<std::size_t(
        OperationContext* opCtx, OplogBuffer* proxy, const OplogBuffer::Value& value)> pushFn) {
    ASSERT_EQUALS(proxy->lastObjectPushed(opCtx), boost::none);
    ASSERT_FALSE(mock->lastObjectPushedCalled);

    auto val = BSON("x" << 1);
    auto numPushed = pushFn(opCtx, proxy, val);
    ASSERT_EQUALS(numPushed, mock->values.size());
    ASSERT_BSONOBJ_EQ(val, mock->values.back());

    auto lastObjPushed = proxy->lastObjectPushed(opCtx);
    ASSERT_NOT_EQUALS(boost::none, lastObjPushed);
    ASSERT_BSONOBJ_EQ(val, *lastObjPushed);
    ASSERT_FALSE(mock->lastObjectPushedCalled);
}

TEST_F(OplogBufferProxyTest, PushAllNonBlockingUpdatesCachedLastObjectPushed) {
    auto pushFn = [](OperationContext* opCtx, OplogBuffer* proxy, const OplogBuffer::Value& value) {
        OplogBuffer::Batch values = {BSON("x" << 2), value};
        proxy->push(opCtx, values.cbegin(), values.cend(), boost::none);
        return values.size();
    };
    _testPushFunctionUpdatesCachedLastObjectPushed(_opCtx, _proxy.get(), _mock, pushFn);
}

TEST_F(OplogBufferProxyTest, PushAllNonBlockingDoesNotUpdateCachedLastObjectPushedOnEmptyBatch) {
    OplogBuffer::Batch values;
    _proxy->push(_opCtx, values.cbegin(), values.cend(), boost::none);
    ASSERT_EQUALS(values.size(), _mock->values.size());

    ASSERT_EQUALS(boost::none, _proxy->lastObjectPushed(_opCtx));
    ASSERT_FALSE(_mock->lastObjectPushedCalled);
}

TEST_F(OplogBufferProxyTest, WaitForDataReturnsTrueImmediatelyIfLastObjectPushedIsCached) {
    OplogBuffer::Batch values = {BSON("x" << 1)};
    _proxy->push(_opCtx, values.cbegin(), values.cend(), boost::none);
    ASSERT_TRUE(_proxy->waitForData(Seconds(10)));
    ASSERT_FALSE(_mock->waitForDataCalled);
    ASSERT_FALSE(_mock->waitForDataUntilCalled);
}

TEST_F(OplogBufferProxyTest, WaitForDataForwardsCallToTargetIfLastObjectPushedIsNotCached) {
    ASSERT_FALSE(_proxy->waitForData(Seconds(10)));
    ASSERT_TRUE(_mock->waitForDataCalled);
    ASSERT_FALSE(_mock->waitForDataUntilCalled);
}

TEST_F(OplogBufferProxyTest, WaitForDataUntilReturnsTrueImmediatelyIfLastObjectPushedIsCached) {
    OplogBuffer::Batch values = {BSON("x" << 1)};
    _proxy->push(_opCtx, values.cbegin(), values.cend(), boost::none);
    ASSERT_TRUE(
        _proxy->waitForDataUntil(Date_t::now() + Seconds(10), Interruptible::notInterruptible()));
    ASSERT_FALSE(_mock->waitForDataUntilCalled);
    ASSERT_FALSE(_mock->waitForDataCalled);
}

TEST_F(OplogBufferProxyTest, WaitForDataUntilForwardsCallToTargetIfLastObjectPushedIsNotCached) {
    ASSERT_FALSE(
        _proxy->waitForDataUntil(Date_t::now() + Seconds(10), Interruptible::notInterruptible()));
    ASSERT_TRUE(_mock->waitForDataUntilCalled);
    ASSERT_FALSE(_mock->waitForDataCalled);
}

TEST_F(OplogBufferProxyTest, TryPopResetsLastPushedObjectIfBufferIsEmpty) {
    auto pushValue = BSON("x" << 1);
    OplogBuffer::Batch values = {pushValue};
    _proxy->push(_opCtx, values.cbegin(), values.cend(), boost::none);
    auto lastPushed = _proxy->lastObjectPushed(_opCtx);
    ASSERT_NOT_EQUALS(boost::none, _proxy->lastObjectPushed(_opCtx));
    ASSERT_BSONOBJ_EQ(pushValue, *lastPushed);

    OplogBuffer::Value poppedValue;
    ASSERT_TRUE(_proxy->tryPop(_opCtx, &poppedValue));
    ASSERT_BSONOBJ_EQ(pushValue, poppedValue);
    ASSERT_EQUALS(boost::none, _proxy->lastObjectPushed(_opCtx));

    // waitForData should forward call to underlying buffer.
    ASSERT_FALSE(_proxy->waitForData(Seconds(10)));
    ASSERT_TRUE(_mock->waitForDataCalled);
}

TEST_F(OplogBufferProxyTest, PeekCachesFrontOfBuffer) {
    OplogBuffer::Value peekValue;
    ASSERT_FALSE(_mock->peekCalled);
    ASSERT_FALSE(_proxy->peek(_opCtx, &peekValue));
    ASSERT_TRUE(_mock->peekCalled);
    ASSERT_TRUE(peekValue.isEmpty());
    _mock->peekCalled = false;

    OplogBuffer::Batch values = {BSON("x" << 1), BSON("x" << 2)};
    _proxy->push(_opCtx, values.cbegin(), values.cend(), boost::none);
    ASSERT_EQUALS(values.size(), _mock->values.size());

    ASSERT_TRUE(_proxy->peek(_opCtx, &peekValue));
    ASSERT_TRUE(_mock->peekCalled);
    ASSERT_BSONOBJ_EQ(values.front(), peekValue);
    _mock->peekCalled = false;
    peekValue = OplogBuffer::Value();

    ASSERT_TRUE(_proxy->peek(_opCtx, &peekValue));
    ASSERT_FALSE(_mock->peekCalled);
    ASSERT_BSONOBJ_EQ(values.front(), peekValue);
}

TEST_F(OplogBufferProxyTest, TryPopClearsCachedFrontValue) {
    OplogBuffer::Batch values = {BSON("x" << 1), BSON("x" << 2)};
    _proxy->push(_opCtx, values.cbegin(), values.cend(), boost::none);
    ASSERT_EQUALS(values.size(), _mock->values.size());

    // Peek and pop first value {x: 1}.
    OplogBuffer::Value peekValue;
    ASSERT_TRUE(_proxy->peek(_opCtx, &peekValue));
    ASSERT_TRUE(_mock->peekCalled);
    ASSERT_BSONOBJ_EQ(values.front(), peekValue);
    _mock->peekCalled = false;
    peekValue = OplogBuffer::Value();

    OplogBuffer::Value poppedValue;
    ASSERT_TRUE(_proxy->tryPop(_opCtx, &poppedValue));
    ASSERT_TRUE(_mock->tryPopCalled);
    ASSERT_BSONOBJ_EQ(values.front(), poppedValue);
    ASSERT_EQUALS(boost::none, _proxy->getLastPeeked_forTest());
    _mock->tryPopCalled = false;
    poppedValue = OplogBuffer::Value();

    // Peek and pop second value {x: 2}.
    ASSERT_TRUE(_proxy->peek(_opCtx, &peekValue));
    ASSERT_TRUE(_mock->peekCalled);
    ASSERT_BSONOBJ_EQ(values.back(), peekValue);
    ASSERT_NOT_EQUALS(boost::none, _proxy->getLastPeeked_forTest());
    _mock->peekCalled = false;
    peekValue = OplogBuffer::Value();

    ASSERT_TRUE(_proxy->tryPop(_opCtx, &poppedValue));
    ASSERT_TRUE(_mock->tryPopCalled);
    ASSERT_BSONOBJ_EQ(values.back(), poppedValue);
    ASSERT_EQUALS(boost::none, _proxy->getLastPeeked_forTest());
    _mock->tryPopCalled = false;
    poppedValue = OplogBuffer::Value();

    // Peek and pop empty buffer.
    ASSERT_FALSE(_proxy->peek(_opCtx, &peekValue));
    ASSERT_TRUE(_mock->peekCalled);
    ASSERT_TRUE(peekValue.isEmpty());
    ASSERT_EQUALS(boost::none, _proxy->getLastPeeked_forTest());

    ASSERT_FALSE(_proxy->tryPop(_opCtx, &poppedValue));
    ASSERT_TRUE(_mock->tryPopCalled);
    ASSERT_TRUE(poppedValue.isEmpty());
    ASSERT_EQUALS(boost::none, _proxy->getLastPeeked_forTest());
}

}  // namespace
