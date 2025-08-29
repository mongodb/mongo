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
#include "mongo/db/extension/sdk/extension_helper.h"
#include "mongo/util/assert_util.h"

namespace mongo::extension::sdk {

using ExtensionFactoryFunc = std::function<std::unique_ptr<class Extension>()>;

struct VersionedExtension {
    const ::MongoExtensionAPIVersion version;
    const ExtensionFactoryFunc factoryFunc;
};

// Custom comparator to use for _versionedExtensions.
struct VersionedExtensionGreaterComparator {
    bool operator()(const VersionedExtension& a, const VersionedExtension& b) const {
        const ::MongoExtensionAPIVersion versionA = a.version;
        const ::MongoExtensionAPIVersion versionB = b.version;

        // Sorts from highest version to lowest version.
        return std::tie(versionA.major, versionA.minor, versionA.patch) >
            std::tie(versionB.major, versionB.minor, versionB.patch);
    }
};

class VersionedExtensionContainer {
public:
    static VersionedExtensionContainer& getInstance() {
        static VersionedExtensionContainer singleton;
        return singleton;
    }

    void registerVersion(const ::MongoExtensionAPIVersion& version,
                         ExtensionFactoryFunc&& factoryFunc) {
        // Check if the insertion returns false, indicating that this is a duplicate version.
        if (!_versionedExtensions.insert(VersionedExtension{version, std::move(factoryFunc)})
                 .second) {
            _hasDuplicateVersion = true;
        }
    }

    VersionedExtension getVersionedExtension(
        const ::MongoExtensionAPIVersionVector* hostVersions) const {
        uassert(10930201,
                "Cannot register duplicate versions of the same extension",
                !_hasDuplicateVersion);

        // Loop from highest version to lowest and return the first compatible extension.
        for (const auto& versionedExtension : _versionedExtensions) {
            if (sdk::isVersionCompatible(hostVersions, &versionedExtension.version)) {
                return versionedExtension;
            }
        }

        uasserted(10930202, "There are no registered extensions compatible with the host version");
    }

private:
    std::set<VersionedExtension, VersionedExtensionGreaterComparator> _versionedExtensions;

    bool _hasDuplicateVersion = false;
};

}  // namespace mongo::extension::sdk
