// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"

namespace mongo {

class Document;

/**
 * Converts a document into an equality query that matches only that document, wrapping any
 * dollar-prefixed field names or values so they are treated as literals rather than operators.
 */
BSONObj convertDocumentIntoQuery(const BSONObj& document);

/**
 * Returns true if 'document' has a top-level field name that starts with '$', or a top-level
 * field whose value is an object with a first field name that starts with '$'. If this returns
 * false, convertDocumentIntoQuery(document.toBson()) returns a BSONObj that is identical to
 * 'document.toBson()', so callers may skip calling it and avoid the Document/BSONObj conversion
 * entirely.
 */
bool containsDollarPrefixedFieldNamesOnTopLevel(const Document& document);

}  // namespace mongo
