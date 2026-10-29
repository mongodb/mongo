// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/profile_cmd_test_utils.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

ProfileCmdRequest buildCmdRequest(const ProfileCmdTestArgs& reqArgs) {
    ProfileCmdRequest req{reqArgs.level};
    req.setSampleRate(reqArgs.sampleRate);
    req.setSlowms(reqArgs.slowms);
    req.setSlowinprogms(reqArgs.slowinprogms);
    req.setFilter(reqArgs.filter);
    return req;
}

void validateCmdResponse(BSONObj resp, const ProfileCmdTestArgs& prevSettings) {
    ASSERT_EQ(resp["was"].Number(), prevSettings.level) << resp;
    ASSERT_EQ(resp["slowms"].Number(), prevSettings.slowms.value_or(kDefaultSlowms)) << resp;
    ASSERT_EQ(resp["slowinprogms"].Number(),
              prevSettings.slowinprogms.value_or(kDefaultSlowInProgMS))
        << resp;
    ASSERT_EQ(resp["sampleRate"].Number(), prevSettings.sampleRate.value_or(kDefaultSampleRate))
        << resp;

    if (prevSettings.filter) {
        ASSERT_BSONOBJ_EQ(resp["filter"].Obj(), *prevSettings.filter->obj);
        ASSERT_TRUE(resp.hasField("note") &&
                    resp["note"].String().starts_with("When a filter expression is set"))
            << resp;
    } else {
        ASSERT_FALSE(resp.hasField("filter")) << resp;
    }
}

void validateProfileSettings(const ProfileCmdTestArgs& req, const ProfileSettings& settings) {
    // Expect the param's default value if not set on the request.
    ASSERT_EQ(serverGlobalParams.slowMS.load(), req.slowms.value_or(kDefaultSlowms));
    ASSERT_EQ(serverGlobalParams.sampleRate.load(), req.sampleRate.value_or(kDefaultSampleRate));

    ASSERT_EQ(settings.level, req.level);
    ASSERT_EQ(settings.slowOpInProgressThreshold,
              Milliseconds(req.slowinprogms.value_or(kDefaultSlowInProgMS)));

    if (req.filter && req.filter->obj) {
        ASSERT_NE(settings.filter, nullptr);
        ASSERT_BSONOBJ_EQ(settings.filter->serialize(), *req.filter->obj);
    } else {
        ASSERT_EQ(settings.filter, nullptr);
    }
}
}  // namespace mongo
