/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/util/buildinfo.h"

#include <climits>

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/version.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#if defined(MONGO_CONFIG_SSL) && (MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL)
#include <openssl/opensslv.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {
BuildInfoOpenSSL generateOpenSSLInfo(const VersionInfoInterface& info) {
    BuildInfoOpenSSL ssl;
#ifdef MONGO_CONFIG_SSL
#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
    ssl.setRunning(info.openSSLVersion());
    ssl.setCompiled(StringData{OPENSSL_VERSION_TEXT});
#elif MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_WINDOWS
    ssl.setRunning("Windows SChannel"_sd);
#elif MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_APPLE
    ssl.setRunning("Apple Secure Transport"_sd);
#else
#error "Unknown SSL Provider"
#endif  // MONGO_CONFIG_SSL_PROVIDER
#else
    ssl.setRunning("disabled"_sd);
    ssl.setCompiled("disabled"_sd);
#endif
    return ssl;
}

BSONObj generateBuildEnvironment(const VersionInfoInterface& info) {
    BSONObjBuilder builder;
    for (auto&& e : info.buildInfo()) {
        if (e.inBuildInfo) {
            builder.append(e.key, e.value);
        }
    }
    return builder.obj();
}

#ifdef __APPLE__
boost::optional<std::string> getSysctlString(const std::string& name) {
    std::string buffer;
    buffer.resize(2048);
    std::size_t sz = buffer.size();
    if (sysctlbyname(name.c_str(), buffer.data(), &sz, nullptr, 0) != 0) {
        auto ec = lastSystemError();
        LOGV2_WARNING(
            9574300, "Failed sysctlbyname", "name"_attr = name, "error"_attr = errorMessage(ec));
        return boost::none;
    }
    if (sz <= 1) {
        return boost::none;
    }
    --sz;  // sz includes the trailing NUL
    buffer.resize(sz);
    return buffer;
}

BuildInfoMacOS generateMacOSInfo() {
    using namespace std::literals;

    BuildInfoMacOS macOS;
    if (auto s = getSysctlString("kern.osproductversion"s))
        macOS.setOsProductVersion(std::move(*s));
    if (auto s = getSysctlString("kern.osrelease"s))
        macOS.setOsRelease(std::move(*s));
    if (auto s = getSysctlString("kern.version"s))
        macOS.setVersion(std::move(*s));
    return macOS;
}
#endif  // defined(__APPLE__)

BuildInfo buildInfoVersionOnly(const VersionInfoInterface& info) {
    BuildInfo reply;
    reply.setVersion(info.version());
    reply.setVersionArray({
        info.majorVersion(),
        info.minorVersion(),
        info.patchVersion(),
        info.extraVersion(),
    });
    return reply;
}

}  // namespace

BuildInfo getBuildInfoVersionOnly() {
    return buildInfoVersionOnly(VersionInfoInterface::instance());
}

BuildInfo getBuildInfo() {
    const auto& info = VersionInfoInterface::instance();

    auto reply = buildInfoVersionOnly(info);
    reply.setGitVersion(info.gitVersion());
#ifdef _WIN32
    reply.setTargetMinOS(info.targetMinOS());
#endif
    reply.setModules(info.modules());
    reply.setAllocator(info.allocator());
    reply.setJavascriptEngine(info.jsEngine());
    reply.setSysinfo("deprecated"_sd);
    reply.setOpenssl(generateOpenSSLInfo(info));
    reply.setBuildEnvironment(generateBuildEnvironment(info));
    reply.setBits(static_cast<int>(sizeof(void*)) * CHAR_BIT);
    reply.setDebug(kDebugBuild);
    reply.setMaxBsonObjectSize(BSONObjMaxUserSize);
#ifdef __APPLE__
    reply.setMacOS(generateMacOSInfo());
#endif

    return reply;
}

}  // namespace mongo
