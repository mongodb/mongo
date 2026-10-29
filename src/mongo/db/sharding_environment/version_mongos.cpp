// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/sharding_environment/version_mongos.h"

#include "mongo/db/log_process_details.h"
#include "mongo/util/version.h"

#include <iostream>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

void logMongosVersionInfo(std::ostream* os) {
    if (os) {
        auto&& vii = VersionInfoInterface::instance();
        *os << mongosVersion(vii) << std::endl;
        vii.logBuildInfo(os);
        *os << std::endl;
    } else {
        logProcessDetails(nullptr);
    }
}

}  // namespace mongo
