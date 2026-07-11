// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * The "MinKey" Javascript object.
 *
 * These are slightly special, in that there is only one MinKey object and
 * whenever you call the constructor to make a new one you just get the
 * "singleton" MinKey from the prototype. See the postInstall for details.
 */
struct [[MONGO_MOD_PUBLIC]] MinKeyInfo : public BaseInfo {
    static void call(JSContext* cx, JS::CallArgs args);
    static void construct(JSContext* cx, JS::CallArgs args);
    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(tojson);
        MONGO_DECLARE_JS_FUNCTION(toJSON);
        MONGO_DECLARE_JS_FUNCTION(hasInstance);
    };

    static const JSFunctionSpec methods[4];

    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);

    static const char* const className;
};

}  // namespace mozjs
}  // namespace mongo
