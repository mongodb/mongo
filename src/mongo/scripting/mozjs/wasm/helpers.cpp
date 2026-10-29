// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/freeOpToJSContext.h"

#include "gc/GCContext.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"

namespace mongo {
namespace mozjs {

JSContext* freeOpToJSContext(JS::GCContext* gcCtx) {
    return gcCtx->runtime()->mainContextFromOwnThread();
}

}  // namespace mozjs
}  // namespace mongo
