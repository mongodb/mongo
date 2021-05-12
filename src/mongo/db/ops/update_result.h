/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"

namespace mongo {

struct UpdateResult {
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
