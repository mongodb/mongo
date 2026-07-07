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

#include "mongo/db/extension/host/host_portal.h"
#include "mongo/db/extension/host/load_stub_parsers.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/host_connector/handle/extension_handle.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/shared_library.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <algorithm>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#ifdef __linux__
#include <unistd.h>
#endif

#include <fmt/format.h>
#ifdef __linux__
#include <sys/stat.h>
#endif

#ifdef MONGO_HOST_EXTENSIONS_COMPATIBLE
#include "mongo/db/extension/host/signature_validator.h"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host {
namespace {

void verifyConfigPathPermissions(const std::string& extensionName, const std::string& path) {
#ifdef __linux__
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    uassert(13011803,
            fmt::format("Loading extension '{}' failed: could not open config path '{}': {}",
                        extensionName,
                        path,
                        errorMessage(lastSystemError())),
            fd >= 0);
    ScopeGuard closeFd = [&] {
        ::close(fd);
    };

    struct stat fileStat;
    uassert(13011800,
            fmt::format("Failed to verify extension config path '{}': fstat failed: {}",
                        path,
                        errorMessage(lastSystemError())),
            ::fstat(fd, &fileStat) == 0);
    uassert(13011801,
            fmt::format("Failed to verify extension config path '{}': must be owned by root or "
                        "the server's user",
                        path),
            fileStat.st_uid == 0 || fileStat.st_uid == ::geteuid());
    uassert(13011802,
            fmt::format("Failed to verify extension config path '{}': must not be writable by "
                        "group or other users",
                        path),
            (fileStat.st_mode & (S_IWGRP | S_IWOTH)) == 0);
#endif
}

host_connector::ExtensionHandle getMongoExtension(SharedLibrary& extensionLib,
                                                  const std::string& extensionPath) {
    StatusWith<get_mongodb_extension_versions_t> swGetVersionsFn =
        extensionLib.getFunctionAs<get_mongodb_extension_versions_t>(
            GET_MONGODB_EXTENSION_VERSIONS_SYMBOL);
    uassert(10615501,
            str::stream() << "Loading extension '" << extensionPath
                          << "' failed: " << swGetVersionsFn.getStatus().reason(),
            swGetVersionsFn.isOK());

    StatusWith<get_mongo_extension_t> swGetExtensionFn =
        extensionLib.getFunctionAs<get_mongo_extension_t>(GET_MONGODB_EXTENSION_SYMBOL);
    uassert(12688600,
            str::stream() << "Loading extension '" << extensionPath
                          << "' failed: " << swGetExtensionFn.getStatus().reason(),
            swGetExtensionFn.isOK());

    // Phase 1: extension publishes the API versions it implements. The contract is noexcept;
    // any thrown exception inside the extension is swallowed at its boundary and surfaces here
    // as an empty version vector, which then fails the negotiation step below.
    ::MongoExtensionAPIVersionVector extensionVersions{.len = 0, .versions = nullptr};
    swGetVersionsFn.getValue()(&extensionVersions);

    // Host-side version negotiation: pick the compatible version of the extension.
    const ::MongoExtensionAPIVersion compatibleVersion =
        selectCompatibleVersion(MONGO_EXTENSION_API_VERSIONS_SUPPORTED, extensionVersions);

    // Phase 2: extension instantiates at the chosen version, receiving the matching
    // HostServices. Currently, a single major version is supported, so HostServicesAdapter::get()
    // is guaranteed to be the compatible API.
    const ::MongoExtension* extension = nullptr;
    invokeCAndConvertStatusToException([&]() {
        return swGetExtensionFn.getValue()(
            compatibleVersion, &host_connector::HostServicesAdapter::get(), &extension);
    });
    uassert(10615503,
            str::stream() << "Failed to load extension '" << extensionPath
                          << "': get_mongodb_extension failed to set an extension",
            extension != nullptr);

    return host_connector::ExtensionHandle{extension};
}
}  // namespace

std::filesystem::path ExtensionLoader::getExtensionConfDir() {
    // In the server, serverGlobalParams.extensionsConfigPath is expected to remain consistent after
    // start-up. However, we avoid caching its value into a static variable to accommodate any
    // potential future unit testing which may rely on mutating the extensionsConfigPath across
    // sequential unit tests.
    if (!serverGlobalParams.extensionsConfigPath.empty()) {
        return std::filesystem::path(serverGlobalParams.extensionsConfigPath);
    }
    // TODO SERVER-127732: Once Atlas has made the changes to provide the extensions config path
    // value to the server, remove this fallback and assert that we received a config value.
    static constexpr std::string_view kExtensionConfigPath = "/etc/mongo/extensions";
    LOGV2(12758900,
          "No extensionsConfigPath was provided; using the default extension config directory",
          "defaultPath"_attr = kExtensionConfigPath);
    return {kExtensionConfigPath};
}

::MongoExtensionAPIVersion selectCompatibleVersion(
    const ::MongoExtensionAPIVersionVector& hostVersions,
    const ::MongoExtensionAPIVersionVector& extensionVersions) {
    uassert(12688601,
            "Failed to load extension: the extension published a null API version list",
            extensionVersions.versions != nullptr);

    uassert(12688602,
            "Failed to load extension: the extension published an empty API version list",
            extensionVersions.len > 0);

    // Sort a copy of the extension's published versions in descending (major, minor) order so we
    // can early-return on the first compatible match.
    std::vector<::MongoExtensionAPIVersion> sortedExtensionVersions(
        extensionVersions.versions, extensionVersions.versions + extensionVersions.len);
    std::sort(sortedExtensionVersions.begin(),
              sortedExtensionVersions.end(),
              [](const ::MongoExtensionAPIVersion& a, const ::MongoExtensionAPIVersion& b) {
                  return std::tie(a.major, a.minor) > std::tie(b.major, b.minor);
              });

    const auto hostVersionsSpan = std::span(hostVersions.versions, hostVersions.len);
    bool sawMatchingMajor = false;
    for (const auto& extVersion : sortedExtensionVersions) {
        for (const auto& hostVersion : hostVersionsSpan) {
            if (hostVersion.major == extVersion.major) {
                sawMatchingMajor = true;
                if (hostVersion.minor >= extVersion.minor) {
                    return extVersion;
                }
            }
        }
    }

    uassert(10615504,
            mongo::str::stream() << "Failed to load extension: no API major version published by "
                                    "the extension matches a host-supported major version",
            sawMatchingMajor);

    uasserted(10615505,
              mongo::str::stream()
                  << "Failed to load extension: extension's API minor version exceeds the host's "
                     "maximum supported minor for every matching major");
}

stdx::unordered_map<std::string, LoadedExtension> ExtensionLoader::loadedExtensions;

bool loadExtensions(const std::vector<std::string>& extensionNames) {
#ifdef MONGO_HOST_EXTENSIONS_COMPATIBLE
    if (!feature_flags::gFeatureFlagExtensionsAPI.isEnabled()) {
        if (!extensionNames.empty()) {
            LOGV2_ERROR(10668500,
                        "Extensions are not allowed with the current configuration. You may need "
                        "to enable featureFlagExtensionsAPI.");
            return false;
        }
        return true;
    }

    // Register fallback stub parsers before loading extensions.
    registerUnloadedExtensionStubParsers();
    SignatureValidator signatureValidator;

    for (const auto& extension : extensionNames) {
        LOGV2(10668501, "Loading extension", "extensionName"_attr = extension);

        try {
            const ExtensionConfig config = ExtensionLoader::loadExtensionConfig(extension);
            ExtensionLoader::load(extension, config, signatureValidator);
        } catch (...) {
            LOGV2_ERROR(10668502,
                        "Error loading extension",
                        "extensionName"_attr = extension,
                        "status"_attr = exceptionToStatus());
            return false;
        }

        LOGV2(10668503, "Successfully loaded extension", "extensionName"_attr = extension);
    }
#else
    LOGV2(11901200, "Loading extensions is not supported on this platform - skipping loading.");
#endif
    return true;
}

ExtensionConfig ExtensionLoader::loadExtensionConfig(const std::string& extensionName) {
    uassert(11031700,
            str::stream() << "Loading extension '" << extensionName
                          << "' failed: Extension name cannot be empty nor contain path separators",
            !extensionName.empty() && extensionName.find('/') == std::string::npos &&
                extensionName.find('\\') == std::string::npos);

    const auto& confDir = getExtensionConfDir();
    const auto confPath = confDir / std::string(extensionName + ".conf");

    uassert(11042900,
            str::stream() << "Loading extension '" << extensionName
                          << "' failed: Expected configuration file not found at '"
                          << confPath.string() << "'",
            std::filesystem::exists(confPath));

    verifyConfigPathPermissions(extensionName, confDir.string());
    verifyConfigPathPermissions(extensionName, confPath.string());

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
          "extensionName"_attr = extensionName,
          "sharedLibraryPath"_attr = config.sharedLibraryPath);

    return config;
}

void ExtensionLoader::load(const std::string& name, const ExtensionConfig& config) {
#ifdef MONGO_HOST_EXTENSIONS_COMPATIBLE
    SignatureValidator signatureValidator;
    return ExtensionLoader::load(name, config, signatureValidator);
#else
    LOGV2(11901201, "Loading extensions is not supported on this platform - skipping loading.");
#endif
}

#ifdef MONGO_HOST_EXTENSIONS_COMPATIBLE
void ExtensionLoader::load(const std::string& name,
                           const ExtensionConfig& config,
                           const SignatureValidator& signatureValidator) {
    uassert(10845400,
            str::stream() << "Loading extension '" << name << "' failed: "
                          << "Extension has already been loaded",
            !loadedExtensions.contains(name));

    const auto& extensionPath = config.sharedLibraryPath;
    uassert(11528800,
            str::stream() << "Loading extension '" << name << "' failed, path:  " << extensionPath
                          << " does not exist.",
            std::filesystem::exists(extensionPath));
    // "When signature validation is enabled, returned handle owns a descriptor pinned to the exact
    // bytes that were verified and its path() is a "/proc/self/fd/N" string, to avoid a window
    // between verification and dlopen where the bytes could be modified. When validation is off,
    //  path() is just 'extensionPath'."
    ValidatedExtension verifiedFile =
        signatureValidator.validateExtensionSignature(name, extensionPath);
    StatusWith<std::unique_ptr<SharedLibrary>> swExtensionLib =
        SharedLibrary::create(verifiedFile.path());
    uassert(10615500,
            str::stream() << "Loading extension '" << name
                          << "' failed: " << swExtensionLib.getStatus().reason(),
            swExtensionLib.isOK());

    // Leak the descriptor so its "/proc/self/fd/N" path stays valid - and its number is never
    // recycled by the next extension's open() - for the lifetime of the loaded library, which is
    // itself kept alive for the process lifetime.
    verifiedFile.leakDescriptor();

    // Add the 'SharedLibrary' pointer to our loaded extensions map to keep it alive for the
    // lifetime of the server.
    loadedExtensions.emplace(name, LoadedExtension{std::move(swExtensionLib.getValue()), config});
    auto& extensionLib = loadedExtensions[name].library;

    host_connector::ExtensionHandle extHandle = getMongoExtension(*extensionLib, extensionPath);

    // Get the max wire version of the server. During unit testing, return max wire version 0.
    const auto& maxWireVersion = TestingProctor::instance().isEnabled()
        ? 0
        : (mongo::WireSpec::getWireSpec(getGlobalServiceContext())
               .getIncomingInternalClient()
               .maxWireVersion);

    std::unique_ptr<HostPortal> hostPortal = std::make_unique<HostPortal>();
    host_connector::HostPortalAdapter portal{extHandle->getVersion(),
                                             maxWireVersion,
                                             YAML::Dump(config.extOptions),
                                             std::move(hostPortal)};
    extHandle->initialize(&portal);
}
#endif

stdx::unordered_map<std::string, ExtensionConfig> ExtensionLoader::getLoadedExtensions() {
    stdx::unordered_map<std::string, ExtensionConfig> result;

    for (const auto& [name, loadedExtension] : loadedExtensions) {
        tassert(11048700,
                str::stream() << "Extension names must be unique, but '" << name
                              << "' already exists",
                !result.contains(name));
        result.insert({name, loadedExtension.config});
    }

    return result;
}

bool ExtensionLoader::isLoaded(const std::string& name) {
    return loadedExtensions.contains(name);
}

void ExtensionLoader::unload_forTest(const std::string& name) {
    if (isLoaded(name)) {
        loadedExtensions.erase(loadedExtensions.find(name));
    }
}

}  // namespace mongo::extension::host
