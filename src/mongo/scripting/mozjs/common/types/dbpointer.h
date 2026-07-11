// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/CallArgs.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * The "DBPointer" Javascript Object
 *
 * These look like:
 * {
 *     id : OID(),
 *     ns : String(),
 * }
 */
struct DBPointerInfo : public BaseInfo {
    static void construct(JSContext* cx, JS::CallArgs args);

    static const char* const className;
};

}  // namespace mozjs
}  // namespace mongo
