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


#include "mongo/util/version.h"


#ifdef MONGO_CONFIG_SSL
#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#endif
#endif

#include <climits>
#include <fmt/format.h>
#include <sstream>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/debug_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {

class FallbackVersionInfo : public VersionInfoInterface {
public:
    int majorVersion() const noexcept final {
        return 0;
    }

    int minorVersion() const noexcept final {
        return 0;
    }

    int patchVersion() const noexcept final {
        return 0;
    }

    int extraVersion() const noexcept final {
        return 0;
    }

    StringData version() const noexcept final {
        return "unknown";
    }

    StringData gitVersion() const noexcept final {
        return "none";
    }

    std::vector<StringData> modules() const final {
        return {"unknown"};
    }

    StringData allocator() const noexcept final {
        return "unknown";
    }

    StringData jsEngine() const noexcept final {
        return "unknown";
    }

    StringData targetMinOS() const noexcept final {
        return "unknown";
    }

    std::vector<BuildInfoField> buildInfo() const final {
        return {};
    }
};

const VersionInfoInterface* globalVersionInfo = nullptr;

}  // namespace

void VersionInfoInterface::enable(const VersionInfoInterface* handler) {
    globalVersionInfo = handler;
}

const VersionInfoInterface& VersionInfoInterface::instance(NotEnabledAction action) noexcept {
    if (globalVersionInfo) {
        return *globalVersionInfo;
    }

    if (action == NotEnabledAction::kFallback) {
        static const auto& fallbackVersionInfo = *new FallbackVersionInfo;

        return fallbackVersionInfo;
    }

    LOGV2_FATAL(40278, "Terminating because valid version info has not been configured");
}

std::string VersionInfoInterface::makeVersionString(StringData binaryName) const {
    return format(FMT_STRING("{} v{}"), binaryName, version());
}

std::string VersionInfoInterface::openSSLVersion(StringData prefix, StringData suffix) const {
#if !defined(MONGO_CONFIG_SSL) || MONGO_CONFIG_SSL_PROVIDER != MONGO_CONFIG_SSL_PROVIDER_OPENSSL
    return "";
#elif MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
    return prefix.toString() + SSLeay_version(SSLEAY_VERSION) + suffix;
#endif
}

void VersionInfoInterface::logTargetMinOS() const {
    LOGV2(23398, "Target operating system minimum version", "targetMinOS"_attr = targetMinOS());
}

void VersionInfoInterface::logBuildInfo(std::ostream* os) const {
    BSONObjBuilder bob;
    bob.append("version", version());
    bob.append("gitVersion", gitVersion());
#if defined(MONGO_CONFIG_SSL) && MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
    bob.append("openSSLVersion", openSSLVersion());
#endif
    bob.append("modules", modules());
    bob.append("allocator", allocator());
    {
        auto envObj = BSONObjBuilder(bob.subobjStart("environment"));
        for (auto&& bi : buildInfo())
            if (bi.inVersion && !bi.value.empty())
                envObj.append(bi.key, bi.value);
    }
    BSONObj obj = bob.done();
    if (os) {
        // If printing to ostream, print a json object with a single "buildInfo" element.
        *os << "Build Info: " << tojson(obj, ExtendedRelaxedV2_0_0, true) << std::endl;
    } else {
        LOGV2(23403, "Build Info", "buildInfo"_attr = obj);
    }
}

std::string formatVersionString(StringData versioned, const VersionInfoInterface& provider) {
    return format(FMT_STRING("{} version v{}"), versioned, provider.version());
}

std::string mongoShellVersion(const VersionInfoInterface& provider) {
    return formatVersionString("MongoDB shell", provider);
}

std::string mongosVersion(const VersionInfoInterface& provider) {
    return formatVersionString("mongos", provider);
}

std::string mongocryptVersion(const VersionInfoInterface& provider) {
    return formatVersionString("mongocrypt", provider);
}

std::string mongodVersion(const VersionInfoInterface& provider) {
    return formatVersionString("db", provider);
}

}  // namespace mongo
