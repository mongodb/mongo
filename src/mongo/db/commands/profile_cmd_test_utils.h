// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/profile_common.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/profile_settings.h"
#include "mongo/util/modules.h"

namespace mongo {

static const int kDefaultSlowms = 200;
static const int kDefaultSlowInProgMS = 1000;
static const double kDefaultSampleRate = 1.0;

/**
 * Struct encapsulating arguments to the profile command for ease of use in tests.
 */
struct ProfileCmdTestArgs {
    int64_t level = 0;
    boost::optional<double> sampleRate = boost::none;
    boost::optional<int64_t> slowms = boost::none;
    boost::optional<int64_t> slowinprogms = boost::none;
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
