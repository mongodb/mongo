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
    req.setFilter(reqArgs.filter);
    return req;
}

void validateCmdResponse(BSONObj resp, const ProfileCmdTestArgs& prevSettings) {
    ASSERT_EQ(resp["was"].Number(), prevSettings.level) << resp;
    ASSERT_EQ(resp["slowms"].Number(), prevSettings.slowms.value_or(kDefaultSlowms)) << resp;
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

    if (req.filter && req.filter->obj) {
        ASSERT_NE(settings.filter, nullptr);
        ASSERT_BSONOBJ_EQ(settings.filter->serialize(), *req.filter->obj);
    } else {
        ASSERT_EQ(settings.filter, nullptr);
    }
}
}  // namespace mongo
