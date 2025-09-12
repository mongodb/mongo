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

#include "mongo/db/extension/host_adapter/extension_handle.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/extension_status.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/shared_library.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <iostream>
#include <stdexcept>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host {
namespace {
// TODO SERVER-110326: Check if we are in a test environment. If so, use /tmp/mongo/extensions.
static const std::filesystem::path kExtensionConfDir{"/etc/mongo/extensions"};

void assertVersionCompatibility(const ::MongoExtensionAPIVersionVector* hostVersions,
                                const ::MongoExtensionAPIVersion& extensionVersion) {
    bool foundCompatibleMajor = false;
    bool foundCompatibleMinor = false;

    for (uint64_t i = 0; i < hostVersions->len; ++i) {
        const auto& hostVersion = hostVersions->versions[i];
        if (hostVersion.major == extensionVersion.major) {
            foundCompatibleMajor = true;
            if (hostVersion.minor >= extensionVersion.minor) {
                foundCompatibleMinor = true;
                break;
            }
        }
    }

    uassert(10615504,
            mongo::str::stream() << "Failed to load extension: Invalid API major version; expected "
                                 << extensionVersion.major
                                 << " to match one of the host major versions",
            foundCompatibleMajor);

    uassert(10615505,
            mongo::str::stream()
                << "Failed to load extension: Incompatible API minor version; expected "
                << extensionVersion.minor
                << " to be no greater than the maximum minor version for major version "
                << extensionVersion.major,
            foundCompatibleMinor);
}

host_adapter::ExtensionHandle getMongoExtension(SharedLibrary& extensionLib,
                                                const std::string& extensionPath) {
    StatusWith<get_mongo_extension_t> swGetExtensionFunction =
        extensionLib.getFunctionAs<get_mongo_extension_t>(GET_MONGODB_EXTENSION_SYMBOL);
    uassert(10615501,
            str::stream() << "Loading extension '" << extensionPath
                          << "' failed: " << swGetExtensionFunction.getStatus().reason(),
            swGetExtensionFunction.isOK());

    const ::MongoExtension* extension = nullptr;
    sdk::enterC([&]() {
        return swGetExtensionFunction.getValue()(&MONGO_EXTENSION_API_VERSIONS_SUPPORTED,
                                                 &extension);
    });
    uassert(10615503,
            str::stream() << "Failed to load extension '" << extensionPath
                          << "': get_mongodb_extension failed to set an extension",
            extension != nullptr);

    return host_adapter::ExtensionHandle{extension};
}
}  // namespace

stdx::unordered_map<std::string, std::unique_ptr<SharedLibrary>> ExtensionLoader::loadedExtensions;

bool loadExtensions(const std::vector<std::string>& extensionPaths) {
    if (extensionPaths.empty()) {
        return true;
    }

    if (!feature_flags::gFeatureFlagExtensionsAPI.isEnabled()) {
        LOGV2_ERROR(10668500,
                    "Extensions are not allowed with the current configuration. You may need to "
                    "enable featureFlagExtensionsAPI.");
        return false;
    }

    for (const auto& extension : extensionPaths) {
        // TODO SERVER-110317: Change 'filePath' to 'extensionName'.
        LOGV2(10668501, "Loading extension", "filePath"_attr = extension);

        try {
            const ExtensionConfig config = ExtensionLoader::loadExtensionConfig(extension);
            ExtensionLoader::load(config);
        } catch (...) {
            LOGV2_ERROR(10668502,
                        "Error loading extension",
                        // TODO SERVER-110317: Change 'filePath' to 'extensionName'.
                        "filePath"_attr = extension,
                        "status"_attr = exceptionToStatus());
            return false;
        }

        // TODO SERVER-110317: Change 'filePath' to 'extensionName'.
        LOGV2(10668503, "Successfully loaded extension", "filePath"_attr = extension);
    }

    return true;
}

ExtensionConfig ExtensionLoader::loadExtensionConfig(const std::string& extensionPath) {
    // TODO SERVER-110317: 'extensionPath' shouldn't represent a path anymore. We can omit the
    // truncation.
    const auto confPath = kExtensionConfDir /
        std::filesystem::path(extensionPath).filename().replace_extension(".conf");
    // TODO SERVER-110326: Remove this once we have proper loading for tests.
    if (!std::filesystem::exists(confPath)) {
        return ExtensionConfig{.sharedLibraryPath = extensionPath,
                               .extOptions = YAML::Node(YAML::NodeType::Map)};
    }
    uassert(11042900,
            str::stream() << "Loading extension '" << extensionPath
                          << "' failed: Expected configuration file not found at '"
                          << confPath.string() << "'",
            std::filesystem::exists(confPath));

    const auto root = [&] {
        try {
            return YAML::LoadFile(confPath.string());
        } catch (const YAML::Exception& e) {
            uasserted(11042901,
                      str::stream() << "Unexpected error while loading extension config file '"
                                    << confPath.string() << "': " << e.what());
        }
        return YAML::Node();
    }();
    uassert(11042902,
            str::stream() << "Invalid extension config file '" << confPath.string()
                          << "': missing required field 'sharedLibraryPath'",
            root[kSharedLibraryPath]);

    ExtensionConfig config;
    config.sharedLibraryPath = root[kSharedLibraryPath].as<std::string>();
    config.extOptions =
        root[kExtensionOptions] ? root[kExtensionOptions] : YAML::Node(YAML::NodeType::Map);

    LOGV2(11042903,
          "Successfully loaded config file",
          "sharedLibraryPath"_attr = config.sharedLibraryPath,
          "extensionOptions"_attr = YAML::Dump(config.extOptions));

    return config;
}

void ExtensionLoader::load(const ExtensionConfig& config) {
    const auto& extensionPath = config.sharedLibraryPath;
    uassert(10845400,
            str::stream() << "Loading extension '" << extensionPath << "' failed: "
                          << "Extension has already been loaded",
            !loadedExtensions.contains(extensionPath));

    StatusWith<std::unique_ptr<SharedLibrary>> swExtensionLib =
        SharedLibrary::create(extensionPath);
    uassert(10615500,
            str::stream() << "Loading extension '" << extensionPath
                          << "' failed: " << swExtensionLib.getStatus().reason(),
            swExtensionLib.isOK());

    // Add the 'SharedLibrary' pointer to our loaded extensions array to keep it alive for the
    // lifetime of the server.
    loadedExtensions.emplace(extensionPath, std::move(swExtensionLib.getValue()));
    auto& extensionLib = loadedExtensions[extensionPath];

    host_adapter::ExtensionHandle extHandle = getMongoExtension(*extensionLib, extensionPath);
    // Validate that the major and minor versions from the extension implementation are compatible
    // with the host API version.
    assertVersionCompatibility(&MONGO_EXTENSION_API_VERSIONS_SUPPORTED, extHandle.getVersion());

    // Get the max wire version of the server. During unit testing, return max wire version 0.
    const auto& maxWireVersion = TestingProctor::instance().isEnabled()
        ? 0
        : (mongo::WireSpec::getWireSpec(getGlobalServiceContext())
               .getIncomingInternalClient()
               .maxWireVersion);

    HostPortal portal{extHandle.getVersion(), maxWireVersion, YAML::Dump(config.extOptions)};
    extHandle.initialize(portal);
}
}  // namespace mongo::extension::host
