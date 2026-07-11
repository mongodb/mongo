// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <map>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Utility functions to get or set boolean flags associated to an arbitrary flag name from/to a
 * WiredTiger storage engine options BSON object.
 *
 * The idea is that for exceptional (workaround) purposes, we can use the storage engine
 * options object as a flexible structure where new fields can be added retroactively,
 * unlike the other parts of the catalog which generally have non-flexible / strict validations.
 * For more information, see: SERVER-91195, SERVER-92186.
 *
 * Flags are persisted in the application-owned metadata (`app_metadata`) key of the WiredTiger
 * configuration string; e.g. setting flag "abc" to true sets it in the BSON object like:
 * {storageEngine: {wiredTiger: {configString: 'app_metadata=(abc=true)'}}}
 */
std::map<std::string_view, boost::optional<bool>> getFlagsFromWiredTigerStorageOptions(
    const BSONObj& storageEngineOptions, const std::vector<std::string_view>& flagNames);

boost::optional<bool> getFlagFromWiredTigerStorageOptions(const BSONObj& storageEngineOptions,
                                                          std::string_view flagName);

BSONObj setFlagsToWiredTigerStorageOptions(
    const BSONObj& storageEngineOptions,
    const std::map<std::string_view, boost::optional<bool>>& flags);

BSONObj setFlagToWiredTigerStorageOptions(const BSONObj& storageEngineOptions,
                                          std::string_view flagName,
                                          boost::optional<bool> flagValue);

}  // namespace mongo
