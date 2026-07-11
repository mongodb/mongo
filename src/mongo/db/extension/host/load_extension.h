// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/platform/shared_library.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <filesystem>
#include <string>

#include <yaml-cpp/yaml.h>

namespace mongo::extension::host {

/**
 * Parsed contents of an extension's .conf file.
 */
struct ExtensionConfig {
    std::string sharedLibraryPath;
    YAML::Node extOptions;
};

/**
 * A loaded extension: the opened SharedLibrary and its config. Kept alive for the process lifetime.
 */
struct LoadedExtension {
    std::unique_ptr<SharedLibrary> library;
    ExtensionConfig config;
};

static ::MongoExtensionAPIVersion supportedVersions[] = {MONGODB_EXTENSION_API_VERSION};

static const ::MongoExtensionAPIVersionVector MONGO_EXTENSION_API_VERSIONS_SUPPORTED = {
    .len = 1, .versions = supportedVersions};


/**
 * Return the compatible extension API version given the vector of supported host API versions.
 * Returns the chosen version on success and uasserts on incompatibility.
 *
 * Selection policy: among the extension's published versions, pick the highest (major, minor)
 * that is compatible with at least one host version. A version V is compatible with host version
 * H iff V.major == H.major && H.minor >= V.minor. The chosen version's minor is the extension's
 * minor (the lower bound between the two sides) which the host must respect when dispatching
 * vtable slots later.
 *
 * Exposed for unit testing; production callers go through ExtensionLoader::load.
 */
::MongoExtensionAPIVersion selectCompatibleVersion(
    const ::MongoExtensionAPIVersionVector& hostVersions,
    const ::MongoExtensionAPIVersionVector& extensionVersions);

/**
 * Load all extensions in the provided array. Returns true if loading is successful, otherwise
 * false.
 */
[[MONGO_MOD_PUBLIC]] bool loadExtensions(const std::vector<std::string>& extensionNames);

class ExtensionLoader {
public:
    /**
     * Loads the corresponding configuration file for the given extension and returns it as an
     * ExtensionConfig.
     */
    static ExtensionConfig loadExtensionConfig(const std::string& extensionName);

    /**
     * Given an extension name and configuration struct, loads the extension, checks for API version
     * compatibility, and calls the extension initialization function.
     */
    static void load(const std::string& name, const ExtensionConfig& config);
#ifdef MONGO_HOST_EXTENSIONS_COMPATIBLE
    static void load(const std::string& name,
                     const ExtensionConfig& config,
                     const class SignatureValidator& signatureValidator);
#endif

    /**
     * Returns the names and configurations of the currently registered extensions.
     */
    static stdx::unordered_map<std::string, ExtensionConfig> getLoadedExtensions();

    /**
     * Returns true if an extension with the given name is loaded.
     */
    static bool isLoaded(const std::string& name);

    /**
     * Unload the extension with the given name if it has been loaded.
     *
     * We do not gracefully support extension unloading at runtime, so this should only be called in
     * tests.
     */
    static void unload_forTest(const std::string& name);

private:
    // Used to keep loaded extension 'SharedLibrary' objects alive for the lifetime of the server
    // and track what extensions have been loaded. Initialized during process initialization and
    // const thereafter. These are intentionally "leaked" on shutdown.
    static stdx::unordered_map<std::string, LoadedExtension> loadedExtensions;

    // Expected YAML config file field names.
    static inline constexpr char kSharedLibraryPath[] = "sharedLibraryPath";
    static inline constexpr char kExtensionOptions[] = "extensionOptions";
};

}  // namespace mongo::extension::host
