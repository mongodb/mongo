// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/load_extension.h"
#include "mongo/util/modules.h"

#include <filesystem>

namespace mongo::extension::host::test_util {

static inline const std::filesystem::path runFilesDir = std::getenv("RUNFILES_DIR");

/**
 * Returns the directory containing extension shared libraries built for tests.
 */
inline std::filesystem::path getExtensionDirectory() {
    return runFilesDir / "_main/src/mongo/db/extension/test_examples";
}

/**
 * Computes the absolute path to a specific test extension shared library.
 */
inline std::filesystem::path getExtensionPath(const std::string& extensionFileName) {
    return getExtensionDirectory() / extensionFileName;
}

/**
 * Constructs a minimal ExtensionConfig pointing at the given test extension.
 *
 * The resulting config has:
 *   - sharedLibraryPath = <resolved path to .so file>
 *   - extOptions = empty YAML map
 */
inline ExtensionConfig makeEmptyExtensionConfig(const std::string& extensionFileName) {
    return ExtensionConfig{.sharedLibraryPath = getExtensionPath(extensionFileName).string(),
                           .extOptions = YAML::Node(YAML::NodeType::Map)};
}

inline const std::string& getPublicKeyPath() {
    static std::string kPublicKeyPath = getExtensionDirectory() / "test_extensions_signing_keys" /
        "test_extensions_signing_public_key.asc";
    return kPublicKeyPath;
}

}  // namespace mongo::extension::host::test_util
