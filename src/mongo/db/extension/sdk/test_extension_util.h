// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {
/**
 * Helper function to assert that the input BSON for an aggregation stage has a 'stageName' object
 * field.
 */
inline mongo::BSONObj validateStageDefinition(mongo::BSONObj stageBson,
                                              const std::string& stageName,
                                              bool checkEmpty = false) {
    sdk_uassert(11165100,
                "Failed to parse " + stageName + ", expected object",
                stageBson.hasField(stageName) && stageBson.getField(stageName).isABSONObj());
    if (checkEmpty) {
        sdk_uassert(11165101,
                    stageName + " stage definition must be an empty object",
                    stageBson.getField(stageName).Obj().isEmpty());
    }
    return stageBson[stageName].Obj();
}
}  // namespace mongo::extension::sdk
