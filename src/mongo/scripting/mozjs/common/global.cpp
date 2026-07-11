// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/scripting/mozjs/common/global.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#ifndef MONGO_MOZJS_WASI_BUILD
#include "mongo/scripting/engine.h"
#else
#include "mongo/scripting/js_regex.h"
#endif
#include "mongo/scripting/mozjs/common/jsstringwrapper.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/util/assert_util.h"
#ifndef MONGO_MOZJS_WASI_BUILD
#include "mongo/util/buildinfo.h"
#endif
#include "mongo/util/version.h"

#include <cstddef>
#include <cstdint>
#include <ostream>

#include <js/CallArgs.h>
#include <js/Conversions.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace mozjs {

#ifdef MONGO_MOZJS_WASI_BUILD
namespace wasm {
extern uint32_t g_wasmJsHeapLimitMB;
}  // namespace wasm
#endif

const JSFunctionSpec GlobalInfo::freeFunctions[7] = {
    MONGO_ATTACH_JS_FUNCTION(sleep),
    MONGO_ATTACH_JS_FUNCTION(gc),
    MONGO_ATTACH_JS_FUNCTION(print),
    MONGO_ATTACH_JS_FUNCTION(version),
    MONGO_ATTACH_JS_FUNCTION(buildInfo),
    MONGO_ATTACH_JS_FUNCTION(getJSHeapLimitMB),
    JS_FS_END,
};

const char* const GlobalInfo::className = "Global";

void GlobalInfo::Functions::print::call(JSContext* cx, JS::CallArgs args) {
    std::ostringstream ss;

    bool first = true;
    for (size_t i = 0; i < args.length(); i++) {
        if (first)
            first = false;
        else
            ss << " ";

        if (args.get(i).isNullOrUndefined()) {
            // failed to get object to convert
            ss << "[unknown type]";
            continue;
        }

        JSStringWrapper jsstr(cx, JS::ToString(cx, args.get(i)));
        ss << jsstr.toStringData();
    }

    args.rval().setUndefined();

    LOGV2_INFO_OPTIONS(
        20162,
        logv2::LogOptions(logv2::LogTag::kPlainShell, logv2::LogTruncation::Disabled),
        "{jsPrint}",
        "jsPrint"_attr = ss.str());
}

void GlobalInfo::Functions::version::call(JSContext* cx, JS::CallArgs args) {
    auto versionString = VersionInfoInterface::instance().version();
    ValueReader(cx, args.rval()).fromStringData(versionString);
}

void GlobalInfo::Functions::buildInfo::call(JSContext* cx, JS::CallArgs args) {
#ifdef MONGO_MOZJS_WASI_BUILD
    // WASI builds don't have getBuildInfo() - return empty object
    BSONObjBuilder b;
    ValueReader(cx, args.rval()).fromBSON(b.obj(), nullptr, false);
#else
    BSONObjBuilder b;
    getBuildInfo().serialize(&b);
    ValueReader(cx, args.rval()).fromBSON(b.obj(), nullptr, false);
#endif
}

void GlobalInfo::Functions::getJSHeapLimitMB::call(JSContext* cx, JS::CallArgs args) {
#ifdef MONGO_MOZJS_WASI_BUILD
    ValueReader(cx, args.rval()).fromDouble(static_cast<double>(wasm::g_wasmJsHeapLimitMB));
#else
    ValueReader(cx, args.rval()).fromDouble(getGlobalScriptEngine()->getJSHeapLimitMB());
#endif
}

void GlobalInfo::Functions::gc::call(JSContext* cx, JS::CallArgs args) {
    getCommonRuntime(cx)->gc();
    args.rval().setUndefined();
}

void GlobalInfo::Functions::sleep::call(JSContext* cx, JS::CallArgs args) {
    uassert(16259,
            "sleep takes a single numeric argument -- sleep(milliseconds)",
            args.length() == 1 && args.get(0).isNumber());

    int64_t duration = ValueWriter(cx, args.get(0)).toInt64();
    getCommonRuntime(cx)->sleep(Milliseconds(duration));

    args.rval().setUndefined();
}

}  // namespace mozjs
}  // namespace mongo
