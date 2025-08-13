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

#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/platform/shared_library.h"
#include "mongo/stdx/unordered_map.h"

#include <string>

namespace mongo::extension::host {

static ::MongoExtensionAPIVersion supportedVersions[] = {MONGODB_EXTENSION_API_VERSION};

static const ::MongoExtensionAPIVersionVector MONGO_EXTENSION_API_VERSIONS_SUPPORTED = {
    .len = 1, .versions = supportedVersions};


/**
 * Load all extensions in the provided array. Returns true if loading is successful, otherwise
 * false.
 */
bool loadExtensions(const std::vector<std::string>& extensionPaths);

class ExtensionLoader {
public:
    /**
     * Given a path to an extension shared library, loads the extension, checks for API version
     * compatibility, and calls the extension initialization function.
     */
    static void load(const std::string& extensionPath);

private:
    // Used to keep loaded extension 'SharedLibrary' objects alive for the lifetime of the server
    // and track what extensions have been loaded. Initialized during process initialization and
    // const thereafter. These are intentionally "leaked" on shutdown.
    static stdx::unordered_map<std::string, std::unique_ptr<SharedLibrary>> loadedExtensions;
};

}  // namespace mongo::extension::host
