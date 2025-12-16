/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
