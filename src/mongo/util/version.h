// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#ifndef UTIL_VERSION_HEADER
#define UTIL_VERSION_HEADER

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class BSONObjBuilder;

/**
 * An interface for accessing version information about the current process. A singleton instance of
 * this interface is expected to be available via the 'instance' method in processes that need to be
 * able to access version information.
 */
class VersionInfoInterface {
public:
    struct BuildInfoField {
        std::string_view key;
        std::string_view value;
        bool inBuildInfo;  // included in buildInfo BSON
        bool inVersion;    // included in --version output
    };

    enum class NotEnabledAction {
        kAbortProcess,
        kFallback,
    };

    VersionInfoInterface(const VersionInfoInterface&) = delete;
    VersionInfoInterface& operator=(const VersionInfoInterface&) = delete;
    virtual ~VersionInfoInterface() = default;

    /**
     * The provided implementation of this interface will be returned by the 'instance' method
     * below. Ownership of the object is not transferred.
     */
    static void enable(const VersionInfoInterface* handler);

    /**
     * Obtain the currently configured instance of the VersionInfoInterface. By default, if this
     * method is called and no implementation has been configured with the 'enable' method above,
     * the process will be terminated. If it is not acceptable to terminate the process, the above
     * 'kFallback' constant can be provided and defaulted information will be provided.
     */
    static const VersionInfoInterface& instance(
        NotEnabledAction action = NotEnabledAction::kAbortProcess) noexcept;

    /**
     * Returns the major version as configured via MONGO_VERSION.
     */
    virtual int majorVersion() const = 0;

    /**
     * Returns the minor version as configured via MONGO_VERSION.
     */
    virtual int minorVersion() const = 0;

    /**
     * Returns the patch version as configured via MONGO_VERSION.
     */
    virtual int patchVersion() const = 0;

    /**
     * Returns the extra version as configured via MONGO_VERSION.
     */
    virtual int extraVersion() const = 0;

    /**
     * Returns a string representation of MONGO_VERSION.
     */
    virtual std::string_view version() const = 0;

    /**
     * Returns a string representation of MONGO_GIT_HASH.
     */
    virtual std::string_view gitVersion() const = 0;

    /**
     * Returns a vector describing the enabled modules.
     */
    virtual std::vector<std::string_view> modules() const = 0;

    /**
     * Returns a string describing the configured memory allocator.
     */
    virtual std::string_view allocator() const = 0;

    /**
     * Returns a string describing the configured javascript engine.
     */
    virtual std::string_view jsEngine() const = 0;

    /**
     * Returns a string describing the minimum requred OS. Note that this method is currently only
     * valid to call when running on Windows.
     */
    virtual std::string_view targetMinOS() const = 0;

    /**
     * Returns build information (e.g. LINKFLAGS, compiler, etc.).
     */
    virtual std::vector<BuildInfoField> buildInfo() const = 0;

    /**
     * Returns the version of OpenSSL in use, if any, adorned with the provided prefix and suffix.
     */
    std::string openSSLVersion(std::string_view prefix = "", std::string_view suffix = "") const;

    /**
     * Uses the provided text to make a pretty representation of the version.
     */
    std::string makeVersionString(std::string_view binaryName) const;

    /**
     * Logs the result of 'targetMinOS', above.
     */
    void logTargetMinOS() const;

    /**
     * Logs similar info to `appendBuildInfo`, suitable for the --version flag or for a
     * startup log message (trimmed for user-friendliness). The `buildInfo` data appear
     * in a subobject mapped to the "environment" key, but with the elements for which
     * inVersion == false removed. Puts to `os` if nonnull, else to LOGV2.
     */
    void logBuildInfo(std::ostream* os) const;

protected:
    constexpr VersionInfoInterface() = default;
};

/**
 * Returns a pretty string describing the provided binary's version.
 */
std::string formatVersionString(std::string_view versioned, const VersionInfoInterface& provider);

/**
 * Returns a pretty string describing the current shell version.
 */
std::string mongoShellVersion(const VersionInfoInterface& provider);

/**
 * Returns a pretty string describing the current mongos version.
 */
std::string mongosVersion(const VersionInfoInterface& provider);

/**
 * Returns a pretty string describing the current mongocrypt version.
 */
std::string mongocryptVersion(const VersionInfoInterface& provider);

/**
 * Returns a pretty string describing the current mongod version.
 */
std::string mongodVersion(const VersionInfoInterface& provider);

}  // namespace mongo

#endif  // UTIL_VERSION_HEADER
