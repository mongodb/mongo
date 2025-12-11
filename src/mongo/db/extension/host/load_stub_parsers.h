/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
