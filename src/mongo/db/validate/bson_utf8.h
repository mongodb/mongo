// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"

namespace mongo {
/**
 * Recursively checks `obj` for invalid UTF-8 strings in any field names or string data.
 * Returns false if any such invalid strings are found.
 */
bool isValidUTF8(const BSONObj& obj);

/**
 * Recursively replaces invalid UTF-8 strings in `obj` in any field names or string data with
 * "\xef\xbf\xbd", which is the UTF-8 encoding of the replacement character U+FFFD.
 * https://en.wikipedia.org/wiki/Specials_(Unicode_block)#Replacement_character
 *
 * This function will return the input BSONObj if there aren't invalid UTF-8 strings. Otherwise, it
 * returns a new BSONObj.
 */
BSONObj checkAndScrubInvalidUTF8(BSONObj obj);
}  // namespace mongo
