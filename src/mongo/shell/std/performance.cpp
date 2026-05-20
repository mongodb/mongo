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

#include "mongo/scripting/mozjs/shell/internal_module_registry.h"

#include <chrono>

#include <jsapi.h>

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/Value.h>

namespace mongo::JSFiles {
extern const JSFile std_performance;
}  // namespace mongo::JSFiles

namespace mongo::mozjs::std_modules {
namespace {

constexpr double kNanosPerMillis = 1e6;
const auto kPerformanceProcessStart = std::chrono::steady_clock::now();

double performanceNowMillis() {
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - kPerformanceProcessStart);
    return static_cast<double>(elapsedNs.count()) / kNanosPerMillis;
}

bool now(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setDouble(performanceNowMillis());
    return true;
}

}  // namespace

bool initializePerformanceBinding(JSContext* cx, JS::HandleObject target) {
    return JS_DefineFunction(cx, target, "now", now, 0, JSPROP_ENUMERATE);
}

MONGO_REGISTER_INTERNAL_MODULE_WITH_SETUP("performance",
                                          initializePerformanceBinding,
                                          &::mongo::JSFiles::std_performance);

}  // namespace mongo::mozjs::std_modules
