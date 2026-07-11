// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/Class.h>
#include <js/PropertySpec.h>

namespace mongo {
namespace mozjs {

/**
 * The global object for all of our JS.
 *
 * This function is super special and it's properties are the globally visible
 * symbol for JS execution.
 */
struct GlobalInfo : public BaseInfo {
    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(sleep);
        MONGO_DECLARE_JS_FUNCTION(gc);
        MONGO_DECLARE_JS_FUNCTION(print);
        MONGO_DECLARE_JS_FUNCTION(version);
        MONGO_DECLARE_JS_FUNCTION(buildInfo);
        MONGO_DECLARE_JS_FUNCTION(getJSHeapLimitMB);
    };

    static const JSFunctionSpec freeFunctions[7];

    static const char* const className;
    static const unsigned classFlags = JSCLASS_GLOBAL_FLAGS;
};

}  // namespace mozjs
}  // namespace mongo
