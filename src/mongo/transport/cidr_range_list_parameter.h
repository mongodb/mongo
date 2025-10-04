/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/versioned_value.h"

#include <string>
#include <variant>
#include <vector>

namespace mongo::transport {

/**
 * Appends a CIDR range list to a bson object. The array will be appended as a subobject using a key
 * with the value of the 'name' parameter.
 */
void appendCIDRRangeListParameter(VersionedValue<CIDRList>& value,
                                  BSONObjBuilder* bob,
                                  StringData name);

/**
 * Parses the 'obj' bson object and sets 'value' if parsing the CIDR range was sucessful. If parsing
 * was not sucessful, an error status from the thrown exception is returned.
 */
Status setCIDRRangeListParameter(VersionedValue<CIDRList>& value, BSONObj obj);

// TODO: SERVER-106468 Define CIDRRangeListParameter here instead of generating the code.

}  // namespace mongo::transport
