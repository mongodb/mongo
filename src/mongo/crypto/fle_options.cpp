// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/init.h"
#include "mongo/crypto/fle_options_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

MONGO_INITIALIZER_WITH_PREREQUISITES(TestingDiagnostics, ("EndStartupOptionStorage"))
(InitializerContext*) {
    if (gUnsupportedDangerousTestingFLEDiagnosticsEnabledAtStartup) {
        LOGV2_OPTIONS(7319001,
                      {logv2::LogTag::kStartupWarnings},
                      "Queryable Encryption Testing behaviors are enabled. This has serious "
                      "implications for both "
                      "performance and security of Queryable Encryption. This configuration is not "
                      "supported.");
    }
}

}  // namespace
}  // namespace mongo
