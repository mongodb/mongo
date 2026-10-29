// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/resumetoken.h"

#include "mongo/db/pipeline/resume_token.h"
#include "mongo/scripting/mozjs/common/types/bson.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/scripting/mozjs/shell/runtime.h"

#include <string>

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

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
    auto* runtime = getCommonRuntime(cx);
    getProto<BSONInfo>(runtime).newObject(&thisValue);
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
