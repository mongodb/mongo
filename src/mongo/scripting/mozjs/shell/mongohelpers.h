// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * The "MongoHelpers" Javascript object.
 *
 * The MongoHelpers object is a special hidden object to attach internal-use
 * javascript code to the global object so that we can do things like access the
 * javascript parser through SpiderMonkey's reflection API, and parse function
 * bodies/expressions from the server code.
 */
struct MongoHelpersInfo : public BaseInfo {
    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);
    static const InstallType installType = InstallType::Private;
    static const char* const className;
};

std::string parseJSFunctionOrExpression(JSContext* cx, std::string_view input);

}  // namespace mozjs
}  // namespace mongo
