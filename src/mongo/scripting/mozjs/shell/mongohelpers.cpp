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

#include "mongo/scripting/mozjs/shell/mongohelpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/parse_function_helper.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/util/assert_util.h"

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

std::string parseJSFunctionOrExpression(JSContext* cx, const StringData input) {
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
