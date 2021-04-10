/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {

static constexpr auto kMaxTimeMSOpOnlyField = "maxTimeMSOpOnly";

// A constant by which 'maxTimeMSOpOnly' values are allowed to exceed the max allowed value for
// 'maxTimeMS'.  This is because mongod and mongos server processes add a small amount to the
// 'maxTimeMS' value they are given before passing it on as 'maxTimeMSOpOnly', to allow for
// clock precision.
static constexpr auto kMaxTimeMSOpOnlyMaxPadding = 100LL;

/**
 * Parses maxTimeMS from the BSONElement containing its value.
 * The field name of the 'maxTimeMSElt' is used to determine what maximum value to enforce for
 * the provided max time. 'maxTimeMSOpOnly' needs a slightly higher max value than regular
 * 'maxTimeMS' to account for the case where a user provides the max possible value for
 * 'maxTimeMS' to one server process (mongod or mongos), then that server process passes the max
 * time on to another server as 'maxTimeMSOpOnly', but after adding a small amount to the max
 * time to account for clock precision.  This can push the 'maxTimeMSOpOnly' sent to the mongod
 * over the max value allowed for users to provide. This is safe because 'maxTimeMSOpOnly' is
 * only allowed to be provided for internal intra-cluster requests.
 */
StatusWith<int> parseMaxTimeMS(BSONElement maxTimeMSElt);

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
int32_t parseMaxTimeMSForIDL(BSONElement maxTimeMSElt);

}  // namespace mongo
