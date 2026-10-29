// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/schema/encrypt_schema_types.h"
#include "mongo/util/modules.h"

namespace mongo::parsers::matcher::schema {
/**
 * This is a deserializer for the EncryptSchemaKeyId IDL type. Creates a EncryptSchemaKeyId from a
 * BSONElement.
 *
 * Returns an error if the element cannot be parsed.
 */

EncryptSchemaKeyId parseEncryptSchemaKeyId(const BSONElement& element);

/**
 * This is a deserializer for the BSONTypeSet IDL type. Creates a BSONTypeSet from a BSONElement.
 *
 * Returns an error if the element cannot be parsed.
 */
BSONTypeSet parseBSONTypeSet(const BSONElement& element);
}  // namespace mongo::parsers::matcher::schema
