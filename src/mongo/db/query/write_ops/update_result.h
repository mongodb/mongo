// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

struct [[MONGO_MOD_PUBLIC]] UpdateResult {
    UpdateResult(bool existing,
                 bool modifiers,
                 unsigned long long numDocsModified,
                 unsigned long long numMatched,
                 const BSONObj& upsertedObject,
                 bool dotsAndDollarsField = false);

    std::string toString() const;

    // True if at least one pre-existing document was modified.
    const bool existing;

    // True if this was a modifier-style update, not a replacement update or a pipeline-style
    // update.
    const bool modifiers;

    const long long numDocsModified;

    const long long numMatched;

    // If either the operation was not an upsert, or the upsert did not result in an insert, then
    // this is the empty object. If an insert occurred as the result of an upsert operation, then
    // this is a single-element object containing the _id of the document inserted.
    BSONObj upsertedId;

    BSONObj requestedDocImage;

    // True if the documents updated/inserted contain '.'/'$' field.
    const bool containsDotsAndDollarsField;
};

}  // namespace mongo
