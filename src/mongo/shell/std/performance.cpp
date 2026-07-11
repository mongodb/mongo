// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
