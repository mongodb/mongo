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

#include "mongo/base/string_data.h"

#include <string>

/**
 * The 'redact' methods defined below should be used to redact possibly sensitive
 * information when operating the server in 'redact' mode.
 *
 * The performance impact of calling redact when not in 'redact' mode should be neglectible.
 *
 * The 'redact' methods are designed to be used as part of our log streams
 * log(), LOG(), warning(), error(), severe() similar to the example below.
 *
 * log() << "My sensitive query is: " << query;
 * log() << "My sensitive query is: " << redact(query);
 */

namespace mongo {

class BSONObj;
class Status;
class DBException;

/**
 *  In 'redact' mode replace all values with '###' and keep keys intact.
 *  In normal mode return objectToRedact.toString().
 */
BSONObj redact(const BSONObj& objectToRedact);

/**
 *  In 'redact mode return '###'.
 *  In normal mode return stringToRedact.
 */
StringData redact(StringData stringToRedact);
inline StringData redact(const char* stringToRedact) {
    return redact(StringData(stringToRedact));
}
inline StringData redact(const std::string& stringToRedact) {
    return redact(StringData(stringToRedact));
}

/**
 *  In 'redact' mode keep status code and replace reason with '###'.
 *  In normal mode return statusToRedact.toString().
 */
std::string redact(const Status& statusToRedact);

/**
 * In 'redact' mode keep exception type and replace causedBy with '###'.
 * In normal mode return exceptionToRedact.toString().
 */
std::string redact(const DBException& exceptionToRedact);

}  // namespace mongo
