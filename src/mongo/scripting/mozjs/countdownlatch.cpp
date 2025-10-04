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

#include <jsapi.h>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/countdownlatch.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace mongo {
namespace mozjs {

const char* const CountDownLatchInfo::className = "CountDownLatch";

const JSFunctionSpec CountDownLatchInfo::methods[5] = {
    MONGO_ATTACH_JS_FUNCTION(_new),
    MONGO_ATTACH_JS_FUNCTION(_await),
    MONGO_ATTACH_JS_FUNCTION(_countDown),
    MONGO_ATTACH_JS_FUNCTION(_getCount),
    JS_FS_END,
};

/**
 * The global CountDownLatch holder.
 *
 * Provides an interface for communicating between JSThread's
 */
class CountDownLatchHolder {
public:
    CountDownLatchHolder() : _counter(0) {}

    int32_t make(int32_t count) {
        uassert(ErrorCodes::JSInterpreterFailure, "argument must be >= 0", count >= 0);
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        int32_t desc = ++_counter;
        _latches.insert(std::make_pair(desc, std::make_shared<CountDownLatch>(count)));

        return desc;
    }

    void await(int32_t desc) {
        auto latch = get(desc);
        stdx::unique_lock<stdx::mutex> lock(latch->mutex);

        while (latch->count != 0) {
            latch->cv.wait(lock);
        }
    }

    void countDown(int32_t desc) {
        auto latch = get(desc);
        stdx::unique_lock<stdx::mutex> lock(latch->mutex);

        if (latch->count > 0)
            latch->count--;

        if (latch->count == 0)
            latch->cv.notify_all();
    }

    int32_t getCount(int32_t desc) {
        auto latch = get(desc);
        stdx::unique_lock<stdx::mutex> lock(latch->mutex);

        return latch->count;
    }

private:
    /**
     * Latches for communication between threads
     */
    struct CountDownLatch {
        CountDownLatch(int32_t count) : count(count) {}

        stdx::mutex mutex;
        stdx::condition_variable cv;
        int32_t count;
    };

    std::shared_ptr<CountDownLatch> get(int32_t desc) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        auto iter = _latches.find(desc);
        uassert(ErrorCodes::JSInterpreterFailure,
                "not a valid CountDownLatch descriptor",
                iter != _latches.end());

        return iter->second;
    }

    using Map = stdx::unordered_map<int32_t, std::shared_ptr<CountDownLatch>>;

    stdx::mutex _mutex;
    Map _latches;
    int32_t _counter;
};

namespace {
CountDownLatchHolder globalCountDownLatchHolder;
}  // namespace

/**
 * The argument for _new is a count value. We restrict it to be a 32 bit integer.
 *
 * The argument for _await/_countDown/_getCount is an id for CountDownLatch instance returned from
 * _new call. It must be a 32 bit integer.
 */
auto uassertGet(JS::CallArgs args, unsigned int i) {
    uassert(ErrorCodes::JSInterpreterFailure, "need exactly one argument", args.length() == 1);

    auto int32Arg = args.get(i);
    if (int32Arg.isDouble()) {
        uassert(ErrorCodes::JSInterpreterFailure,
                "argument must not be an NaN",
                !int32Arg.isDouble() || !std::isnan(int32Arg.toDouble()));
        auto val = int32Arg.toDouble();
        uassert(ErrorCodes::JSInterpreterFailure,
                "argument must be a 32 bit integer",
                INT_MIN <= val && val <= INT_MAX);

        return static_cast<int32_t>(val);
    }

    uassert(
        ErrorCodes::JSInterpreterFailure, "argument must be a 32 bit integer", int32Arg.isInt32());

    return int32Arg.toInt32();
}

void CountDownLatchInfo::Functions::_new::call(JSContext* cx, JS::CallArgs args) {
    args.rval().setInt32(globalCountDownLatchHolder.make(uassertGet(args, 0)));
}

void CountDownLatchInfo::Functions::_await::call(JSContext* cx, JS::CallArgs args) {
    globalCountDownLatchHolder.await(uassertGet(args, 0));

    args.rval().setUndefined();
}

void CountDownLatchInfo::Functions::_countDown::call(JSContext* cx, JS::CallArgs args) {
    globalCountDownLatchHolder.countDown(uassertGet(args, 0));

    args.rval().setUndefined();
}

void CountDownLatchInfo::Functions::_getCount::call(JSContext* cx, JS::CallArgs args) {
    args.rval().setInt32(globalCountDownLatchHolder.getCount(uassertGet(args, 0)));
}

/**
 * We have to do this odd dance here because we need the methods from
 * CountDownLatch to be installed in a plain object as enumerable properties.
 * This is due to the way CountDownLatch is invoked, specifically after being
 * transmitted across our js fork(). So we can't inherit and can't rely on the
 * type. Practically, we also end up wrapping up all of these functions in pure
 * js variants that call down, which makes them bson <-> js safe.
 */
void CountDownLatchInfo::postInstall(JSContext* cx,
                                     JS::HandleObject global,
                                     JS::HandleObject proto) {
    auto objPtr = JS_NewPlainObject(cx);
    uassert(ErrorCodes::JSInterpreterFailure, "Failed to JS_NewPlainObject", objPtr);

    JS::RootedObject obj(cx, objPtr);
    ObjectWrapper objWrapper(cx, obj);
    ObjectWrapper protoWrapper(cx, proto);

    JS::RootedValue val(cx);
    for (auto iter = methods; iter->name; ++iter) {
        invariant(!iter->name.isSymbol());
        ObjectWrapper::Key key(iter->name.string());
        protoWrapper.getValue(key, &val);
        objWrapper.setValue(key, val);
    }

    val.setObjectOrNull(obj);
    ObjectWrapper(cx, global).setValue(CountDownLatchInfo::className, val);
}

}  // namespace mozjs
}  // namespace mongo
