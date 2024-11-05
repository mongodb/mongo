/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <js/CallArgs.h>
#include <js/TypeDecls.h>
#include <string>

#include <js/PropertySpec.h>

#include "mongo/db/pipeline/resume_token.h"
#include "mongo/scripting/mozjs/bson.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"  // IWYU pragma: keep

namespace mongo {
namespace mozjs {

const JSFunctionSpec ResumeTokenDataUtility::freeFunctions[2] = {
    MONGO_ATTACH_JS_FUNCTION(decodeResumeToken),
    JS_FS_END,
};

const char* const ResumeTokenDataUtility::className = "ResumeTokenDataUtility";

constexpr const char* kHighWaterMarkResumeTokenType = "highWaterMarkResumeTokenType";
constexpr const char* kEventResumeTokenType = "eventResumeTokenType";

void ResumeTokenDataUtility::Functions::decodeResumeToken::call(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::BadValue, "decodeResumeToken takes 1 argument.", args.length() == 1);

    BSONObj encodedResumeTokenBson = ValueWriter(cx, args.get(0)).toBSON();
    const auto resumeTokenDataBson = ResumeToken::parse(encodedResumeTokenBson).getData().toBSON();

    JS::RootedObject thisValue(cx);
    auto scope = getScope(cx);
    scope->getProto<BSONInfo>().newObject(&thisValue);
    BSONInfo::make(cx, &thisValue, std::move(resumeTokenDataBson), nullptr, true);

    args.rval().setObjectOrNull(thisValue);
}

void ResumeTokenDataUtility::postInstall(JSContext* cx,
                                         JS::HandleObject global,
                                         JS::HandleObject proto) {
    JS::RootedValue undef(cx);
    undef.setUndefined();

    // The following code initialises two constants which will be visible in the global JS scope.
    // The constants correspond to 'ResumeTokenData::TokenType' enum type values and
    // they are used to improve readability of test assertions involving the 'tokenType' field.

    // highWaterMarkResumeTokenType
    if (!JS_DefineProperty(
            cx,
            global,
            kHighWaterMarkResumeTokenType,
            JS::RootedValue(cx, JS::Int32Value(ResumeTokenData::kHighWaterMarkToken)),
            JSPROP_READONLY | JSPROP_PERMANENT)) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_DefineProperty");
    }

    // eventResumeTokenType
    if (!JS_DefineProperty(cx,
                           global,
                           kEventResumeTokenType,
                           JS::RootedValue(cx, JS::Int32Value(ResumeTokenData::kEventToken)),
                           JSPROP_READONLY | JSPROP_PERMANENT)) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to JS_DefineProperty");
    }
}

}  // namespace mozjs
}  // namespace mongo
