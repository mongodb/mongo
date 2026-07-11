// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/version.h"


#ifdef MONGO_CONFIG_SSL
#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#endif
#endif

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/debug_util.h"

#include <climits>
#include <sstream>
#include <string_view>

#include <fmt/format.h>

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

    std::string_view version() const noexcept final {
        return "unknown";
    }

    std::string_view gitVersion() const noexcept final {
        return "none";
    }

    std::vector<std::string_view> modules() const final {
        return {"unknown"};
    }

    std::string_view allocator() const noexcept final {
        return "unknown";
    }

    std::string_view jsEngine() const noexcept final {
        return "unknown";
    }

    std::string_view targetMinOS() const noexcept final {
        return "unknown";
    }

    std::vector<BuildInfoField> buildInfo() const final {
        return {};
    }
};

const VersionInfoInterface*& globalVersionInfoPtr() {
    static const VersionInfoInterface* p = nullptr;
    return p;
}

}  // namespace

void VersionInfoInterface::enable(const VersionInfoInterface* handler) {
    globalVersionInfoPtr() = handler;
}

const VersionInfoInterface& VersionInfoInterface::instance(NotEnabledAction action) noexcept {
    if (auto p = globalVersionInfoPtr()) {
        return *p;
    }

    if (action == NotEnabledAction::kFallback) {
        static const auto& fallbackVersionInfo = *new FallbackVersionInfo;

        return fallbackVersionInfo;
    }

    LOGV2_FATAL(40278, "Terminating because valid version info has not been configured");
}

std::string VersionInfoInterface::makeVersionString(std::string_view binaryName) const {
    return fmt::format("{} v{}", binaryName, version());
}

std::string VersionInfoInterface::openSSLVersion(std::string_view prefix,
                                                 std::string_view suffix) const {
#if !defined(MONGO_CONFIG_SSL) || MONGO_CONFIG_SSL_PROVIDER != MONGO_CONFIG_SSL_PROVIDER_OPENSSL
    return "";
#elif MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
    return fmt::format("{}{}{}", prefix, SSLeay_version(SSLEAY_VERSION), suffix);
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
    bob.append("javascriptEngine", jsEngine());
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

std::string formatVersionString(std::string_view versioned, const VersionInfoInterface& provider) {
    return fmt::format("{} version v{}", versioned, provider.version());
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
