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


#include "mongo/scripting/mozjs/global.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/buildinfo.h"
#include "mongo/util/duration.h"
#include "mongo/util/version.h"

#include <cstddef>
#include <cstdint>
#include <ostream>

#include <js/CallArgs.h>
#include <js/Conversions.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace mozjs {

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
    BSONObjBuilder b;
    getBuildInfo().serialize(&b);
    ValueReader(cx, args.rval()).fromBSON(b.obj(), nullptr, false);
}

void GlobalInfo::Functions::getJSHeapLimitMB::call(JSContext* cx, JS::CallArgs args) {
    ValueReader(cx, args.rval()).fromDouble(mongo::getGlobalScriptEngine()->getJSHeapLimitMB());
}

void GlobalInfo::Functions::gc::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    scope->gc();

    args.rval().setUndefined();
}

void GlobalInfo::Functions::sleep::call(JSContext* cx, JS::CallArgs args) {
    uassert(16259,
            "sleep takes a single numeric argument -- sleep(milliseconds)",
            args.length() == 1 && args.get(0).isNumber());

    auto scope = getScope(cx);
    int64_t duration = ValueWriter(cx, args.get(0)).toInt64();
    scope->sleep(Milliseconds(duration));

    args.rval().setUndefined();
}

}  // namespace mozjs
}  // namespace mongo
