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

#include "mongo/db/commands/profile_common.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/profile_settings.h"

namespace mongo {

static const int kDefaultSlowms = 200;
static const double kDefaultSampleRate = 1.0;

/**
 * Struct encapsulating arguments to the profile command for ease of use in tests.
 */
struct ProfileCmdTestArgs {
    int64_t level = 0;
    boost::optional<double> sampleRate = boost::none;
    boost::optional<int64_t> slowms = boost::none;
    boost::optional<ObjectOrUnset> filter = boost::none;
};

/**
 * Build the profile command object from the test args.
 */
ProfileCmdRequest buildCmdRequest(const ProfileCmdTestArgs& reqArgs);

/**
 * Validate the response from the profile command given the previous profile settings.
 */
void validateCmdResponse(BSONObj resp, const ProfileCmdTestArgs& prevSettings);

/**
 * Ensure that the correct server state was updated per the profile request.
 */
void validateProfileSettings(const ProfileCmdTestArgs& req, const ProfileSettings& settings);
}  // namespace mongo
