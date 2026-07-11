// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * The "Timestamp" Javascript object.
 *
 * Represents a bson timestamp that looks like:
 *
 * {
 *     t : Double,
 *     i : Double,
 * }
 */
struct TimestampInfo : public BaseInfo {
    static void construct(JSContext* cx, JS::CallArgs args);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(toJSON);
    };

    static const JSFunctionSpec methods[2];

    static const char* const className;

    static Timestamp getValidatedValue(JSContext* cx, JS::HandleObject obj);
};

}  // namespace mozjs
}  // namespace mongo
