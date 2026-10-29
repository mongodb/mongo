// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Returns a status to indicate whether or not 'element' is a valid _id field for storage in a
 * collection.
 */
Status validIdField(const mongo::BSONElement& element);
}  // namespace mongo
