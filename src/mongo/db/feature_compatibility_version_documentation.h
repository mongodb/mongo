// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::feature_compatibility_version_documentation {
[[MONGO_MOD_PUBLIC]] constexpr inline std::string_view kReleaseNotesRoot{
    "https://docs.mongodb.com/master/release-notes"};

[[MONGO_MOD_PUBLIC]] inline std::string compatibilityLink() {
    return fmt::format(                                //
        "{}/{}-compatibility/#feature-compatibility",  //
        kReleaseNotesRoot,                             //
        multiversion::toString(multiversion::GenericFCV::kLastLTS));
}

[[MONGO_MOD_PUBLIC]] inline std::string upgradeLink() {
    return fmt::format(               //
        "{}/{}/#upgrade-procedures",  //
        kReleaseNotesRoot,            //
        multiversion::toString(multiversion::GenericFCV::kLastLTS));
}

}  // namespace mongo::feature_compatibility_version_documentation
