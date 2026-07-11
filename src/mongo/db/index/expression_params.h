// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/hasher.h"
#include "mongo/util/modules.h"


namespace mongo {

class CollatorInterface;

namespace ExpressionParams {

void parseHashParams(const BSONObj& infoObj, int* versionOut, BSONObj* keyPattern);

}  // namespace ExpressionParams

}  // namespace mongo
