/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;

namespace auth {

/**
 * Sets the keys used by authenticateInternalClient - these should be a vector of raw passwords,
 * they will be digested and prepped appropriately by authenticateInternalClient depending
 * on what mechanism is used.
 */
void setInternalAuthKeys(const std::vector<std::string>& keys);

/**
 * Sets the parameters for non-password based internal authentication.
 */
void setInternalUserAuthParams(BSONObj obj);

/**
 * Returns whether there are multiple keys that will be tried while authenticating an internal
 * client (used for logging a startup warning).
 */
bool hasMultipleInternalAuthKeys();

/**
 * Returns whether there are any internal auth data set.
 */
bool isInternalAuthSet();

/**
 * Returns the AuthDB used by internal authentication.
 */
std::string getInternalAuthDB();

/**
 * Returns the internal auth sasl parameters.
 */
BSONObj getInternalAuthParams(size_t idx, StringData mechanism);

/**
 * Create a BSON document for internal authentication.
 */
BSONObj createInternalX509AuthDocument(boost::optional<StringData> userName = boost::none);

}  // namespace auth
}  // namespace mongo
