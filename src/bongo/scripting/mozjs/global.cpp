/**
 * Copyright (C) 2015 BongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "bongo/platform/basic.h"

#include "bongo/scripting/mozjs/global.h"

#include <js/Conversions.h>

#include "bongo/base/init.h"
#include "bongo/logger/logger.h"
#include "bongo/logger/logstream_builder.h"
#include "bongo/scripting/engine.h"
#include "bongo/scripting/mozjs/implscope.h"
#include "bongo/scripting/mozjs/jsstringwrapper.h"
#include "bongo/scripting/mozjs/objectwrapper.h"
#include "bongo/scripting/mozjs/valuereader.h"
#include "bongo/util/version.h"

namespace bongo {
namespace mozjs {

const JSFunctionSpec GlobalInfo::freeFunctions[6] = {
    BONGO_ATTACH_JS_FUNCTION(gc),
    BONGO_ATTACH_JS_FUNCTION(print),
    BONGO_ATTACH_JS_FUNCTION(version),
    BONGO_ATTACH_JS_FUNCTION(buildInfo),
    BONGO_ATTACH_JS_FUNCTION(getJSHeapLimitMB),
    JS_FS_END,
};

const char* const GlobalInfo::className = "Global";

namespace {

logger::MessageLogDomain* jsPrintLogDomain;

}  // namespace

void GlobalInfo::Functions::print::call(JSContext* cx, JS::CallArgs args) {
    logger::LogstreamBuilder builder(jsPrintLogDomain, getThreadName(), logger::LogSeverity::Log());
    std::ostream& ss = builder.stream();

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
    ss << std::endl;

    args.rval().setUndefined();
}

void GlobalInfo::Functions::version::call(JSContext* cx, JS::CallArgs args) {
    auto versionString = VersionInfoInterface::instance().version();
    ValueReader(cx, args.rval()).fromStringData(versionString);
}

void GlobalInfo::Functions::buildInfo::call(JSContext* cx, JS::CallArgs args) {
    BSONObjBuilder b;
    VersionInfoInterface::instance().appendBuildInfo(&b);
    ValueReader(cx, args.rval()).fromBSON(b.obj(), nullptr, false);
}

void GlobalInfo::Functions::getJSHeapLimitMB::call(JSContext* cx, JS::CallArgs args) {
    ValueReader(cx, args.rval()).fromDouble(bongo::getGlobalScriptEngine()->getJSHeapLimitMB());
}

void GlobalInfo::Functions::gc::call(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    scope->gc();

    args.rval().setUndefined();
}

BONGO_INITIALIZER(JavascriptPrintDomain)(InitializerContext*) {
    jsPrintLogDomain = logger::globalLogManager()->getNamedDomain("javascriptOutput");
    return Status::OK();
}

}  // namespace mozjs
}  // namespace bongo
