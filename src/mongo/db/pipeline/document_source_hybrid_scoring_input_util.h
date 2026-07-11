// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Validates that:
 *  (a) there is at least one pipeline,
 *  (b) that each element in the object is an array of objects.
 *  (c) the names don't contain any special characters.
 */
Status validatePipelinesObject(const BSONObj& pipelines);

}  // namespace mongo
