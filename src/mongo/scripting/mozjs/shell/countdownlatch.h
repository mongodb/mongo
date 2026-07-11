// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * The "CountDownLatch" javascript object.
 *
 * Installs a global "CountDownLatch" object with associated methods.
 *
 * Note that there is only one instance of this class and it is used to
 * communicate between different C++ threads.
 */
struct CountDownLatchInfo : public BaseInfo {
    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(_new);
        MONGO_DECLARE_JS_FUNCTION(_await);
        MONGO_DECLARE_JS_FUNCTION(_countDown);
        MONGO_DECLARE_JS_FUNCTION(_getCount);
    };

    static const JSFunctionSpec methods[5];

    static const char* const className;
    static const InstallType installType = InstallType::Private;

    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);
};

}  // namespace mozjs
}  // namespace mongo
