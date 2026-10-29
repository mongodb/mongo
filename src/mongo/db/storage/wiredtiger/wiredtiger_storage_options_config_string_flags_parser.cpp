// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_storage_options_config_string_flags_parser.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/pcre.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include <fmt/format.h>

namespace mongo {

const static StaticImmortal<pcre::Regex> appMetadataRegex(
    R"re((?<=^|,)\s*(?:app_metadata|\"app_metadata\")\s*[=:]\s*[({[]\s*)re");

static pcre::Regex flagMatchRegex(std::string_view flagName) {
    // This check is overly strict, but it suffices for now and ensures that both:
    // - The flag name is a valid WiredTiger identifier, and
    // - It can be used in the regular expression without needing to escape it
    invariant(std::all_of(flagName.begin(), flagName.end(), ctype::isAlpha));

    // Some examples of possible matches:
    // `flag=false`
    // `flag:true`
    // `flag`
    // `     "flag"  =   false     `
    // `     "flag"     `
    return pcre::Regex(fmt::format(
        R"re((?<=[,({{[])\s*(?:{0}|\"{0}\")(?:\s*[=:]\s*(true|false))?\s*(?=[,)}}\]]))re",
        flagName));
}

static std::map<std::string_view, boost::optional<bool>> getFlagsFromWtConfigStringAppMetadata(
    const std::string& configString, const std::vector<std::string_view>& flagNames) {
    std::map<std::string_view, boost::optional<bool>> flags;

    for (const auto& flagName : flagNames) {
        auto flagRegex = flagMatchRegex(flagName);
        auto matches = flagRegex.matchView(configString);
        flags.emplace(flagName,
                      matches ? boost::optional<bool>(matches[1] == "" || matches[1] == "true")
                              : boost::none);
    }

    return flags;
}

std::map<std::string_view, boost::optional<bool>> getFlagsFromWiredTigerStorageOptions(
    const BSONObj& storageEngineOptions, const std::vector<std::string_view>& flagNames) {
    auto configString = WiredTigerUtil::getConfigStringFromStorageOptions(storageEngineOptions);
    return getFlagsFromWtConfigStringAppMetadata(configString.value_or(""), flagNames);
}

boost::optional<bool> getFlagFromWiredTigerStorageOptions(const BSONObj& storageEngineOptions,
                                                          std::string_view flagName) {
    if (storageEngineOptions.isEmpty()) {
        return boost::none;
    }
    return getFlagsFromWiredTigerStorageOptions(storageEngineOptions, {flagName})[flagName];
}

// Finds or adds the 'app_metadata=(...)' struct inside a WiredTiger config string
// Returns the position inside the struct (after the delimiter, before the first key-value)
static size_t findOrAddAppMetadataStructToConfigString(std::string& configString) {
    auto matches = appMetadataRegex->matchView(configString);
    if (!matches)
        configString += configString.empty() ? "app_metadata=()" : ",app_metadata=()";
    return matches ? (matches[0].data() + matches[0].size() - configString.data())
                   : configString.size() - 1;
}

// Expand a [pos, len) range inside a config string to include a leading or trailing comma separator
static void expandRangeToIncludeSeparator(const std::string& configString,
                                          size_t& pos,
                                          size_t& len) {
    if (pos > 0 && configString[pos - 1] == ',') {
        pos--;
        len++;
    } else if (pos + len < configString.size() && configString[pos + len] == ',') {
        len++;
    }
}

static void setFlagsToWtConfigStringAppMetadata(
    std::string& configString, const std::map<std::string_view, boost::optional<bool>>& flags) {
    auto metadataPos = findOrAddAppMetadataStructToConfigString(configString);

    for (const auto& [flagName, flagValue] : flags) {
        auto flagRegex = flagMatchRegex(flagName);
        auto matches = flagRegex.matchView(configString, {}, metadataPos);
        if (matches) {
            size_t pos = matches[0].data() - configString.data(), len = matches[0].size();

            if (flagValue.has_value()) {  // Replace existing flag
                auto flagItem = fmt::format("{}={}", flagName, *flagValue);
                configString.replace(pos, len, flagItem);
            } else {  // Unset existing flag
                expandRangeToIncludeSeparator(configString, pos, len);
                configString.erase(pos, len);
            }
        } else if (flagValue.has_value()) {  // Add new flag
            auto metadataEmpty = strchr(")]}", configString[metadataPos]) != nullptr;
            auto flagItem = fmt::format("{}={}{}", flagName, *flagValue, metadataEmpty ? "" : ",");
            configString.insert(metadataPos, flagItem);
        }
    }
}

BSONObj setFlagsToWiredTigerStorageOptions(
    const BSONObj& storageEngineOptions,
    const std::map<std::string_view, boost::optional<bool>>& flags) {
    auto configString =
        WiredTigerUtil::getConfigStringFromStorageOptions(storageEngineOptions).value_or("");
    setFlagsToWtConfigStringAppMetadata(configString, flags);

    // Both for safety, and because the regex-based parser can not handle some theoretical cases,
    // sanity check that the resulting string is a valid WiredTiger configuration string
    auto configStringObj = BSON(WiredTigerUtil::kConfigStringField << configString);
    tassert(9218600,
            "The resulting WiredTiger configuration string is not valid",
            WiredTigerUtil::checkTableCreationOptions(configStringObj.firstElement()).isOK());

    return WiredTigerUtil::setConfigStringToStorageOptions(storageEngineOptions, configString);
}

BSONObj setFlagToWiredTigerStorageOptions(const BSONObj& storageEngineOptions,
                                          std::string_view flagName,
                                          boost::optional<bool> flagValue) {
    return setFlagsToWiredTigerStorageOptions(storageEngineOptions, {{flagName, flagValue}});
}

}  // namespace mongo
