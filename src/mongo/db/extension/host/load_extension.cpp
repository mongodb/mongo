/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/extension/host/load_extension.h"

#include "mongo/db/extension/public/api.h"
#include "mongo/platform/shared_library.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace mongo::extension::host {

void loadExtension(const std::string& extensionPath) {
    // TODO SERVER-106685:
    // SharedLibrary is an RAII type that will unload the library when it goes out of scope.
    // However, the extension API requires that the library remains loaded throughout the
    // lifetime of the process. We should keep a pointer to the SharedLibrary alive for the lifetime
    // of a node.
    StatusWith<std::unique_ptr<SharedLibrary>> swExtensionLib =
        SharedLibrary::create(extensionPath);
    uassert(10615500,
            str::stream() << "Failed to load extension '" << extensionPath
                          << "': " << swExtensionLib.getStatus().reason(),
            swExtensionLib.isOK());

    StatusWith<get_mongo_extension_t> swGetExtensionFunction =
        swExtensionLib.getValue()->getFunctionAs<get_mongo_extension_t>(
            GET_MONGODB_EXTENSION_SYMBOL);
    uassert(10615501,
            str::stream() << "Failed to load extension '" << extensionPath
                          << "': " << swExtensionLib.getStatus().reason(),
            swGetExtensionFunction.isOK());

    const MongoExtension* extension = swGetExtensionFunction.getValue()();
    uassert(10615503,
            str::stream() << "Failed to load extension '" << extensionPath
                          << "': get_mongodb_extension returned null",
            extension != nullptr);

    // Validate that the major and minor versions from the extension implementation are compatible
    // with the host API version.
    uassert(10615504,
            str::stream() << "Failed to load extension '" << extensionPath
                          << "': Invalid API major version; expected "
                          << MONGODB_EXTENSION_API_MAJOR_VERSION << ", received "
                          << extension->version.major,
            extension->version.major == MONGODB_EXTENSION_API_MAJOR_VERSION);
    uassert(10615505,
            str::stream() << "Failed to load extension '" << extensionPath
                          << "': Invalid API minor version; expected less than or equal to "
                          << MONGODB_EXTENSION_API_MINOR_VERSION << ", received "
                          << extension->version.minor,
            extension->version.minor <= MONGODB_EXTENSION_API_MINOR_VERSION);
    uassert(10615506,
            str::stream() << "Failed to load extension '" << extensionPath
                          << "': initialize function is not defined",
            extension->initialize != nullptr);

    extension->initialize();
}
}  // namespace mongo::extension::host
