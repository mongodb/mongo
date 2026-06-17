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
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/util/modules.h"

#include <vector>

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
        return std::tie(versionA.major, versionA.minor) > std::tie(versionB.major, versionB.minor);
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

    /**
     * Returns the list of registered API versions, in descending order. This method is used by
     * get_mongodb_extension_versions during version negotiation.
     */
    std::vector<::MongoExtensionAPIVersion> getVersionsList() const {
        std::vector<::MongoExtensionAPIVersion> result;
        result.reserve(_versionedExtensions.size());
        for (const auto& entry : _versionedExtensions) {
            result.push_back(entry.version);
        }
        return result;
    }

    /**
     * Returns the registered extension whose version exactly matches 'version'. The host is
     * responsible for choosing a version from the set previously published via
     * 'get_mongodb_extension_versions', so an exact match is expected to succeed; the
     * tripwire-style assert here exists only to catch SDK/host protocol violations.
     */
    VersionedExtension getVersionedExtension(const ::MongoExtensionAPIVersion& version) const {
        sdk_uassert(10930201,
                    "Cannot register duplicate versions of the same extension",
                    !_hasDuplicateVersion);
        for (const auto& entry : _versionedExtensions) {
            if (entry.version.major == version.major && entry.version.minor == version.minor) {
                return entry;
            }
        }
        // We should never arrive here. It is technically undefined behaviour for callers to request
        // a version that the extension doesn't implement.
        sdk_tasserted(
            10930202,
            "Host requested an extension version that was not registered by the extension");
        return VersionedExtension{.version = MONGODB_EXTENSION_API_VERSION,
                                  .factoryFunc = ExtensionFactoryFunc()};
    }

private:
    std::set<VersionedExtension, VersionedExtensionGreaterComparator> _versionedExtensions;

    bool _hasDuplicateVersion = false;
};

}  // namespace mongo::extension::sdk
