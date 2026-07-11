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
 * The "DB" Javascript object.
 *
 * This maps to the 'db' global variable you can call db.COLLECTION_NAME.X() on
 * in the shell.
 *
 * Its major magic is in its getProperty() callback, which threads through to
 * a getCollection method installed in js
 */
struct [[MONGO_MOD_PUBLIC]] DBInfo : public BaseInfo {
    static void construct(JSContext* cx, JS::CallArgs args);
    static void resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp);

    static const char* const className;
};

}  // namespace mozjs
}  // namespace mongo
