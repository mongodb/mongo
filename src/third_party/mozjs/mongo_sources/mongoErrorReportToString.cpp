
/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "jsexn.h"

#include "mozilla/Sprintf.h"

#include <string.h>

#include "jsapi.h"
#include "jsnum.h"
#include "jstypes.h"
#include "jsutil.h"

#include "gc/FreeOp.h"
#include "gc/Marking.h"
#include "js/CharacterEncoding.h"
#include "js/Wrapper.h"
#include "util/StringBuffer.h"
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
#include "vm/JSObject-inl.h"
#include "vm/SavedStacks-inl.h"

#include "mongo/scripting/mozjs/mongoErrorReportToString.h"

using namespace js;
using namespace js::gc;

JSString*
mongoErrorReportToString(JSContext* cx, JSErrorReport* reportp)
{
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
