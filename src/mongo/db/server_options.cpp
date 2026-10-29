// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/server_options.h"

#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"
#include "mongo/util/version/releases.h"

#include <mutex>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

/**
 * This struct represents global configuration data for the server.  These options get set from
 * the command line and are used inline in the code.  Note that much shared code uses this
 * struct, which is why it is here in its own file rather than in the same file as the code that
 * sets it via the command line, which would pull in more dependencies.
 */
ServerGlobalParams serverGlobalParams;

std::string ServerGlobalParams::getPortSettingHelpText() {
    return str::stream() << "Specify port number - " << serverGlobalParams.port << " by default";
}

void ServerGlobalParams::MutableFCV::setVersionFromFCVDocument(
    const FeatureCompatibilityVersionDocument& fcvDoc) {
    auto version = uassertStatusOK(FeatureCompatibilityVersionParser::parse(fcvDoc.toBSON()));
    std::unique_lock lk(_fcvDocMutex);
    _fcvDoc = fcvDoc;
    _version.store(version);
}

void ServerGlobalParams::FCVSnapshot::logFCVWithContext(std::string_view context) const {
    LOGV2_OPTIONS(5853300,
                  {logv2::LogComponent::kReplication},
                  "current featureCompatibilityVersion value",
                  "featureCompatibilityVersion"_attr = multiversion::toString(_version),
                  "context"_attr = context);
}

}  // namespace mongo
