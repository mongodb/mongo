// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/feature_flag.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo::extension::host {
/**
 * The file aggregation_stage_fallback_parsers.json contains a list of aggregation stage names with
 * a message per stage name and an optional feature flag. registerUnloadedExtensionStubParsers()
 * reads that file and registers each stub parser via registerStubParser(). When one of these stages
 * is specified by the user and the feature flag is disabled (or no primary parser is registered),
 * the stub parser will immediately raise an error and include the provided message rather than the
 * generic "unrecognized stage" message.
 *
 * We do this in order to provide helpful error messages to users in situations where specific
 * extensions are not loaded. These stub parsers are specified in a file outside of the server
 * rather than being hardcoded so that if the set of known extensions changes, we can update the
 * error messages without a server release.
 */
void registerUnloadedExtensionStubParsers();

/**
 * Register a fallback stub parser for an extension stage that is not loaded. This is useful when
 * deploying a cluster that may not have extensions loaded but could restart to load extensions
 * later.
 */
void registerStubParser(std::string stageName,
                        std::string message,
                        FeatureFlag* featureFlag = nullptr);
}  // namespace mongo::extension::host
