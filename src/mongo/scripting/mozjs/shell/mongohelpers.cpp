// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/mongohelpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/parse_function_helper.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <jsapi.h>

#include <js/GlobalObject.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const char* const MongoHelpersInfo::className = "MongoHelpers";

namespace {
const char kParseHelperName[] = "__parseJSFunctionOrExpression";
const char kReflectName[] = "Reflect";
}  // namespace

std::string parseJSFunctionOrExpression(JSContext* cx, const std::string_view input) {
    JS::RootedObject proto(cx, getScope(cx)->getProto<MongoHelpersInfo>().getProto());
    std::string out;
    uassert(ErrorCodes::JSInterpreterFailure,
            "Error running javascript function parser",
            mozjs::parseJSFunctionOrExpression(cx, proto, input, &out));
    return out;
}

void MongoHelpersInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {
    ObjectWrapper protoWrapper(cx, proto);
    ObjectWrapper globalWrapper(cx, global);

    // Install the shared parse helper (which also initializes Reflect on the global).
    uassert(ErrorCodes::JSInterpreterFailure,
            "Error installing javascript function parser helper",
            installParseJSFunctionHelper(cx, global));

    // Move Reflect from the global to the MongoHelpers prototype so it is hidden from users
    // but still accessible internally (captured in the parse helper closure).
    JS::RootedValue reflectValue(cx);
    globalWrapper.getValue(kReflectName, &reflectValue);
    globalWrapper.deleteProperty(kReflectName);
    protoWrapper.setValue(kReflectName, reflectValue);

    // Move __parseJSFunctionOrExpression from global to proto so it is not user-visible.
    // The C++ parseJSFunctionOrExpression reads it from proto, not global.
    JS::RootedValue parseHelperValue(cx);
    globalWrapper.getValue(kParseHelperName, &parseHelperValue);
    globalWrapper.deleteProperty(kParseHelperName);
    protoWrapper.setValue(kParseHelperName, parseHelperValue);
}

}  // namespace mozjs
}  // namespace mongo
