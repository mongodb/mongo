// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
