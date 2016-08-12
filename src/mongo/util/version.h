/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#ifndef UTIL_VERSION_HEADER
#define UTIL_VERSION_HEADER

#include <string>
#include <tuple>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"

namespace mongo {

class BSONObjBuilder;

/**
 * An interface for accessing version information about the current process. A singleton instance of
 * this interface is expected to be available via the 'instance' method in processes that need to be
 * able to access version information.
 */
class VersionInfoInterface {
    MONGO_DISALLOW_COPYING(VersionInfoInterface);

public:
    using BuildInfoTuple = std::tuple<StringData, StringData, bool, bool>;

    virtual ~VersionInfoInterface() = default;

    /**
     * The provided implementation of this interface will be returned by the 'instance' method
     * below. Ownership of the object is not transferred.
     */
    static void enable(const VersionInfoInterface* handler);

    enum class NotEnabledAction {
        kAbortProcess,
        kFallback,
    };

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
    virtual int majorVersion() const noexcept = 0;

    /**
     * Returns the minor version as configured via MONGO_VERSION.
     */
    virtual int minorVersion() const noexcept = 0;

    /**
     * Returns the patch version as configured via MONGO_VERSION.
     */
    virtual int patchVersion() const noexcept = 0;

    /**
     * Returns the extra version as configured via MONGO_VERSION.
     */
    virtual int extraVersion() const noexcept = 0;

    /**
     * Returns a string representation of MONGO_VERSION.
     */
    virtual StringData version() const noexcept = 0;

    /**
     * Returns a string representation of MONGO_GIT_HASH.
     */
    virtual StringData gitVersion() const noexcept = 0;

    /**
     * Returns a vector describing the enabled modules.
     */
    virtual std::vector<StringData> modules() const = 0;

    /**
     * Returns a string describing the configured memory allocator.
     */
    virtual StringData allocator() const noexcept = 0;

    /**
     * Returns a string describing the configured javascript engine.
     */
    virtual StringData jsEngine() const noexcept = 0;

    /**
     * Returns a string describing the minimum requred OS. Note that this method is currently only
     * valid to call when running on Windows.
     */
    virtual StringData targetMinOS() const noexcept = 0;

    /**
     * Returns a vector of tuples describing build information (e.g. LINKFLAGS, compiler, etc.).
     */
    virtual std::vector<BuildInfoTuple> buildInfo() const = 0;

    /**
     * Returns the version of OpenSSL in use, if any, adorned with the provided prefix and suffix.
     */
    std::string openSSLVersion(StringData prefix = "", StringData suffix = "") const;

    /**
     * Returns true if the running version has the same major and minor version as the provided
     * string. Note that the minor version is checked, despite the name of this function.
     */
    bool isSameMajorVersion(const char* otherVersion) const noexcept;

    /**
     * Uses the provided text to make a pretty representation of the version.
     */
    std::string makeVersionString(StringData binaryName) const;

    /**
     * Appends the information associated with 'buildInfo', above, to the given builder.
     */
    void appendBuildInfo(BSONObjBuilder* result) const;

    /**
     * Logs the result of 'targetMinOS', above.
     */
    void logTargetMinOS() const;

    /**
     * Logs the result of 'buildInfo', above.
     */
    void logBuildInfo() const;

protected:
    constexpr VersionInfoInterface() = default;
};

/**
 * Returns a pretty string describing the current shell version.
 */
std::string mongoShellVersion(const VersionInfoInterface& provider);

/**
 * Returns a pretty string describing the current mongos version.
 */
std::string mongosVersion(const VersionInfoInterface& provider);

/**
 * Returns a pretty string describing the current mongod version.
 */
std::string mongodVersion(const VersionInfoInterface& provider);

}  // namespace mongo

#endif  // UTIL_VERSION_HEADER
