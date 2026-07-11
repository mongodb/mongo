// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <string>

namespace mongo::cluster::cmd::translations {

inline StringMap<std::string> clusterCommandTranslations = {
    {"abortTransaction", "clusterAbortTransaction"},
    {"aggregate", "clusterAggregate"},
    {"bulkWrite", "clusterBulkWrite"},
    {"commitTransaction", "clusterCommitTransaction"},
    {"delete", "clusterDelete"},
    {"find", "clusterFind"},
    {"getMore", "clusterGetMore"},
    {"insert", "clusterInsert"},
    {"update", "clusterUpdate"}};

[[MONGO_MOD_PUBLIC]] inline BSONObj replaceCommandNameWithClusterCommandName(BSONObj cmdObj) {
    auto cmdName = cmdObj.firstElement().fieldNameStringData();
    auto newNameIt = clusterCommandTranslations.find(cmdName);
    uassert(6349501,
            fmt::format("Cannot use unsupported command {} with cluster transaction API", cmdName),
            newNameIt != clusterCommandTranslations.end());

    return BSONObjBuilder().appendElementsRenamed(cmdObj, BSON(newNameIt->second << 1)).obj();
}

}  // namespace mongo::cluster::cmd::translations
