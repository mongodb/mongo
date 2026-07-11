// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/matchable.h"

namespace mongo {

BSONMatchableDocument::BSONMatchableDocument(const BSONObj& obj) : _obj(obj) {
    _iteratorUsed = false;
}

BSONMatchableDocument::~BSONMatchableDocument() {}
}  // namespace mongo
