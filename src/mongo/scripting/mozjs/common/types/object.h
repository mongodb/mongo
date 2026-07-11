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
 * Adds some methods onto the JS type "Object"
 *
 * Note that this installs "overNative", so we don't actually do anything other
 * than layer a couple of our own functions on top of the existing prototype.
 */
struct ObjectInfo : public BaseInfo {
    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(bsonsize);
    };

    static const JSFunctionSpec methods[2];

    static const char* const className;

    static const InstallType installType = InstallType::OverNative;
};

}  // namespace mozjs
}  // namespace mongo
