// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace mozjs {

/**
 * The "Error" Javascript object.
 *
 * Note that this installs over native.  We only use this to grab the error
 * prototype early in case users overwrite it.
 */
struct ErrorInfo : public BaseInfo {
    static const char* const className;

    static const InstallType installType = InstallType::OverNative;
};

}  // namespace mozjs
}  // namespace mongo
