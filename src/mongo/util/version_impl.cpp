/*    Copyright 2016 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/version.h"

#include "mongo/base/init.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

#define MONGO_UTIL_VERSION_CONSTANTS_H_WHITELISTED
#include "mongo/util/version_constants.h"

namespace mongo {
namespace {

const class : public VersionInfoInterface {
public:
    int majorVersion() const noexcept final {
        return version::kMajorVersion;
    }

    int minorVersion() const noexcept final {
        return version::kMinorVersion;
    }

    int patchVersion() const noexcept final {
        return version::kPatchVersion;
    }

    int extraVersion() const noexcept final {
        return version::kExtraVersion;
    }

    StringData version() const noexcept final {
        return version::kVersion;
    }

    StringData gitVersion() const noexcept final {
        return version::kGitVersion;
    }

    std::vector<StringData> modules() const final {
        return version::kModulesList;
    }

    StringData allocator() const noexcept final {
        return version::kAllocator;
    }

    StringData jsEngine() const noexcept final {
        return version::kJsEngine;
    }

    StringData targetMinOS() const noexcept final {
#if defined(_WIN32)
#if (NTDDI_VERSION >= NTDDI_WIN7)
        return "Windows 7/Windows Server 2008 R2";
#else
#error This targeted Windows version is not supported
#endif  // NTDDI_VERSION
#else
        severe() << "VersionInfoInterface::targetMinOS is only available for Windows";
        fassertFailed(40277);
#endif
    }

    std::vector<BuildInfoTuple> buildInfo() const final {
        return version::kBuildEnvironment;
    }

} kInterpolatedVersionInfo{};

MONGO_INITIALIZER_GENERAL(EnableVersionInfo,
                          MONGO_NO_PREREQUISITES,
                          ("BeginStartupOptionRegistration", "GlobalLogManager"))
(InitializerContext*) {
    VersionInfoInterface::enable(&kInterpolatedVersionInfo);
    return Status::OK();
}

}  // namespace
}  // namespace mongo
