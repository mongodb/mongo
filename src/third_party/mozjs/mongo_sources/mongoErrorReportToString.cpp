// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "jsexn.h"

#include "mozilla/Sprintf.h"

#include <string.h>

#include "jsapi.h"
#include "jsnum.h"
#include "jstypes.h"

#include "gc/Marking.h"
#include "js/CharacterEncoding.h"
#include "js/Wrapper.h"
#include "mozilla/StringBuffer.h"
#include "vm/ErrorObject.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/SavedStacks.h"
#include "vm/SelfHosting.h"
#include "vm/StringType.h"

#include "vm/ErrorObject-inl.h"
#include "vm/SavedStacks-inl.h"

#include "mongo/scripting/mozjs/mongoErrorReportToString.h"

using namespace js;
using namespace js::gc;

JSString* mongoErrorReportToString(JSContext* cx, JSErrorReport* reportp) {
    /*
     * We do NOT want to use GetErrorTypeName() here because it will not do the
     * "right thing" for JSEXN_INTERNALERR.  That is, the caller of this API
     * expects that "InternalError: " will be prepended but GetErrorTypeName
     * goes out of its way to avoid this.
     */
    JSExnType type = static_cast<JSExnType>(reportp->exnType);
    RootedString str(cx);
    if (type != JSEXN_WARN && type != JSEXN_NOTE)
        str = ClassName(GetExceptionProtoKey(type), cx);

    /*
     * If "str" is null at this point, that means we just want to use
     * message without prefixing it with anything.
     */
    if (str) {
        RootedString separator(cx, JS_NewUCStringCopyN(cx, u": ", 2));
        if (!separator)
            return nullptr;
        str = ConcatStrings<CanGC>(cx, str, separator);
        if (!str)
            return nullptr;
    }

    RootedString message(cx, reportp->newMessageString(cx));
    if (!message)
        return nullptr;

    if (!str)
        return message;

    return ConcatStrings<CanGC>(cx, str, message);
}
