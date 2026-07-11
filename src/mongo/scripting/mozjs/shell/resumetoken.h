// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/PropertySpec.h>

namespace mongo {
namespace mozjs {

/**
 * Utility class offering helper methods for managing
 * resume tokens in JavaScript tests.
 */
struct ResumeTokenDataUtility : public BaseInfo {

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(decodeResumeToken);
    };

    static const JSFunctionSpec freeFunctions[2];

    static const char* const className;

    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);
};

}  // namespace mozjs
}  // namespace mongo
