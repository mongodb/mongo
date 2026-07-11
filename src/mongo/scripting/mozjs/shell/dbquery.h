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
 * The "DBQuery" Javascript object.
 *
 * This represents the result of a find() and uses its getProperty() callback
 * to shim operator[]. I.e. db.test.find()[4]
 */
struct DBQueryInfo : public BaseInfo {
    static void construct(JSContext* cx, JS::CallArgs args);
    static void resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp);


    static const char* const className;
};

}  // namespace mozjs
}  // namespace mongo
