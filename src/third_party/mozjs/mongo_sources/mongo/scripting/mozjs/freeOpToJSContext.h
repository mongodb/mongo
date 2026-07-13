// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "jsapi.h"

namespace mongo {
namespace mozjs {

JSContext* freeOpToJSContext(class JS::GCContext* gcCtx);

}  // namespace mozjs
}  // namespace mongo
