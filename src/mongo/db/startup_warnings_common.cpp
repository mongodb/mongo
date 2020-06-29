/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/startup_warnings_common.h"

#include <boost/filesystem/operations.hpp>
#include <fstream>

#include "mongo/client/authenticate.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/version.h"

namespace mongo {

#ifdef _WIN32
bool CheckPrivilegeEnabled(const wchar_t* name) {
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
        auto str = errnoWithPrefix("Failed to LookupPrivilegeValue");
        LOGV2_WARNING(4718701, "{str}", "str"_attr = str);
        return false;
    }

    // Get the access token for the current process.
    HANDLE accessToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &accessToken)) {
        auto str = errnoWithPrefix("Failed to OpenProcessToken");
        LOGV2_WARNING(4718702, "{str}", "str"_attr = str);
        return false;
    }

    const auto accessTokenGuard = makeGuard([&] { CloseHandle(accessToken); });

    BOOL ret;
    PRIVILEGE_SET privileges;
    privileges.PrivilegeCount = 1;
    privileges.Control = PRIVILEGE_SET_ALL_NECESSARY;

    privileges.Privilege[0].Luid = luid;
    privileges.Privilege[0].Attributes = 0;

    if (!PrivilegeCheck(accessToken, &privileges, &ret)) {
        auto str = errnoWithPrefix("Failed to PrivilegeCheck");
        LOGV2_WARNING(4718703, "{str}", "str"_attr = str);
        return false;
    }

    return ret;
}

#endif

//
// system warnings
//
void logCommonStartupWarnings(const ServerGlobalParams& serverParams) {
    // each message adds a leading and a trailing newline

    {
        auto&& vii = VersionInfoInterface::instance();
        if ((vii.minorVersion() % 2) != 0) {
            LOGV2_WARNING_OPTIONS(
                22117,
                {logv2::LogTag::kStartupWarnings},
                "This is a development version of MongoDB. Not recommended for production",
                "version"_attr = vii.version());
        }
    }

    if (serverParams.authState == ServerGlobalParams::AuthState::kUndefined) {
        LOGV2_WARNING_OPTIONS(22120,
                              {logv2::LogTag::kStartupWarnings},
                              "Access control is not enabled for the database. Read and write "
                              "access to data and configuration is unrestricted");
    }

    const bool is32bit = sizeof(int*) == 4;
    if (is32bit) {
        LOGV2_WARNING_OPTIONS(
            22123, {logv2::LogTag::kStartupWarnings}, "This 32-bit MongoDB binary is deprecated");
    }

#ifdef MONGO_CONFIG_SSL
    if (sslGlobalParams.sslAllowInvalidCertificates) {
        LOGV2_WARNING_OPTIONS(
            22124,
            {logv2::LogTag::kStartupWarnings},
            "While invalid X509 certificates may be used to connect to this server, they will not "
            "be considered permissible for authentication");
    }

    if (sslGlobalParams.sslAllowInvalidHostnames) {
        LOGV2_WARNING_OPTIONS(
            22128,
            {logv2::LogTag::kStartupWarnings},
            "This server will not perform X.509 hostname validation. This may allow your server to "
            "make or accept connections to untrusted parties");
    }
#endif

    /*
     * We did not add the message to startupWarningsLog as the user can not
     * specify a sslCAFile parameter from the shell
     */
    if (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled &&
#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
        sslGlobalParams.sslCertificateSelector.empty() &&
#endif
        sslGlobalParams.sslCAFile.empty()) {
#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
        LOGV2_WARNING(22132,
                      "No client certificate validation can be performed since no CA file has been "
                      "provided and no sslCertificateSelector has been specified. Please specify "
                      "an sslCAFile parameter");
#else
        LOGV2_WARNING(22133,
                      "No client certificate validation can be performed since no CA file has been "
                      "provided. Please specify an sslCAFile parameter");
#endif
    }

#if defined(_WIN32) && !defined(_WIN64)
    // Warn user that they are running a 32-bit app on 64-bit Windows
    BOOL wow64Process;
    BOOL retWow64 = IsWow64Process(GetCurrentProcess(), &wow64Process);
    if (retWow64 && wow64Process) {
        LOGV2_OPTIONS(22135,
                      {logv2::LogTag::kStartupWarnings},
                      "This is a 32-bit MongoDB binary running on a 64-bit operating system. "
                      "Switch to a 64-bit build of MongoDB to support larger databases");
    }
#endif

#ifdef _WIN32
    if (!CheckPrivilegeEnabled(SE_INC_WORKING_SET_NAME)) {
        LOGV2_OPTIONS(
            4718704,
            {logv2::LogTag::kStartupWarnings},
            "SeIncreaseWorkingSetPrivilege privilege is not granted to the process. Secure memory "
            "allocation for SCRAM and/or Encrypted Storage Engine may fail.");
    }
#endif

#if !defined(_WIN32)
    if (getuid() == 0) {
        LOGV2_WARNING_OPTIONS(
            22138,
            {logv2::LogTag::kStartupWarnings},
            "You are running this process as the root user, which is not recommended");
    }
#endif

    if (serverParams.bind_ips.empty()) {
        LOGV2_WARNING_OPTIONS(
            22140,
            {logv2::LogTag::kStartupWarnings},
            "This server is bound to localhost. Remote systems will be unable to connect to this "
            "server. Start the server with --bind_ip <address> to specify which IP addresses it "
            "should serve responses from, or with --bind_ip_all to bind to all interfaces. If this "
            "behavior is desired, start the server with --bind_ip 127.0.0.1 to disable this "
            "warning");
    }

    if (auth::hasMultipleInternalAuthKeys()) {
        LOGV2_WARNING_OPTIONS(
            22147,
            {logv2::LogTag::kStartupWarnings},
            "Multiple keys specified in security key file. If cluster key file rollover is not in "
            "progress, only one key should be specified in the key file");
    }
}
}  // namespace mongo
