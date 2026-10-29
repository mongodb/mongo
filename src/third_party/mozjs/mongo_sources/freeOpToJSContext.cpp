// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "gc/GCContext.h"
#include "jsapi.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"

#include "mongo/scripting/mozjs/freeOpToJSContext.h"

namespace mongo {
namespace mozjs {

JSContext* freeOpToJSContext(JS::GCContext* gcCtx) {
    return gcCtx->runtime()->mainContextFromOwnThread();
}

}  // namespace mozjs
}  // namespace mongo
