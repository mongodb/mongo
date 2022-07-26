/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/pipeline/change_stream_expired_pre_image_remover.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class ChangeStreamPreImageExpirationPolicyTest : public ServiceContextTest {
public:
    ChangeStreamPreImageExpirationPolicyTest() {
        ChangeStreamOptionsManager::create(getServiceContext());
    }

    std::unique_ptr<ChangeStreamOptions> populateChangeStreamPreImageOptions(
        stdx::variant<std::string, std::int64_t> expireAfterSeconds) {
        PreAndPostImagesOptions preAndPostImagesOptions;
        preAndPostImagesOptions.setExpireAfterSeconds(expireAfterSeconds);

        auto changeStreamOptions = std::make_unique<ChangeStreamOptions>();
        changeStreamOptions->setPreAndPostImages(std::move(preAndPostImagesOptions));

        return changeStreamOptions;
    }

    void setChangeStreamOptionsToManager(OperationContext* opCtx,
                                         ChangeStreamOptions& changeStreamOptions) {
        auto& changeStreamOptionsManager = ChangeStreamOptionsManager::get(opCtx);
        ASSERT_EQ(changeStreamOptionsManager.setOptions(opCtx, changeStreamOptions).getStatus(),
                  ErrorCodes::OK);
    }

    bool isExpiredPreImage(const Timestamp& preImageTs,
                           const Date_t& preImageOperationTime,
                           const boost::optional<Date_t>& preImageExpirationTime,
                           const Timestamp& earliestOplogEntryTimestamp) {
        change_stream_pre_image_helpers::PreImageAttributes preImageAttributes{
            UUID::gen(), preImageTs, preImageOperationTime};
        return preImageAttributes.isExpiredPreImage(preImageExpirationTime,
                                                    earliestOplogEntryTimestamp);
    }
};

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageExpirationTimeWithValidIntegralValue) {
    auto opCtx = cc().makeOperationContext();
    const int64_t expireAfterSeconds = 10;

    auto changeStreamOptions = populateChangeStreamPreImageOptions(expireAfterSeconds);
    setChangeStreamOptionsToManager(opCtx.get(), *changeStreamOptions.get());

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds =
        change_stream_pre_image_helpers::getPreImageExpirationTime(opCtx.get(), currentTime);
    ASSERT(receivedExpireAfterSeconds);
    ASSERT_EQ(*receivedExpireAfterSeconds, currentTime - Seconds(expireAfterSeconds));
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageExpirationTimeWithUnsetValue) {
    auto opCtx = cc().makeOperationContext();

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds =
        change_stream_pre_image_helpers::getPreImageExpirationTime(opCtx.get(), currentTime);
    ASSERT_FALSE(receivedExpireAfterSeconds);
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageExpirationTimeWithOffValue) {
    auto opCtx = cc().makeOperationContext();

    auto changeStreamOptions = populateChangeStreamPreImageOptions("off");
    setChangeStreamOptionsToManager(opCtx.get(), *changeStreamOptions.get());

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds =
        change_stream_pre_image_helpers::getPreImageExpirationTime(opCtx.get(), currentTime);
    ASSERT_FALSE(receivedExpireAfterSeconds);
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, preImageShouldHaveExpiredWithOlderTimestamp) {
    ASSERT_TRUE(
        isExpiredPreImage(Timestamp(Seconds(100000), 0U) /* preImageTs */,
                          Date_t::now() /* preImageOperationTime */,
                          Date_t::now() /* preImageExpirationTime */,
                          Timestamp(Seconds(100000), 1U)) /* earliestOplogEntryTimestamp */);
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, preImageShouldNotHaveExpired) {
    ASSERT_FALSE(
        isExpiredPreImage(Timestamp(Seconds(100000), 1U) /* preImageTs */,
                          Date_t::now() + Seconds(1) /* preImageOperationTime */,
                          Date_t::now() /* preImageExpirationTime */,
                          Timestamp(Seconds(100000), 0U)) /* earliestOplogEntryTimestamp */);
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, preImageShouldHaveExpiredWithOlderOperationTime) {
    ASSERT_TRUE(
        isExpiredPreImage(Timestamp(Seconds(100000), 1U) /* preImageTs */,
                          Date_t::now() /* preImageOperationTime */,
                          Date_t::now() + Seconds(1) /* preImageExpirationTime */,
                          Timestamp(Seconds(100000), 0U)) /* earliestOplogEntryTimestamp */);
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest,
       preImageShouldNotHaveExpiredWithNullExpirationTime) {
    ASSERT_TRUE(
        isExpiredPreImage(Timestamp(Seconds(100000), 0U) /* preImageTs */,
                          Date_t::now() /* preImageOperationTime */,
                          boost::none /* preImageExpirationTime */,
                          Timestamp(Seconds(100000), 1U)) /* earliestOplogEntryTimestamp */);
}

}  // namespace
}  // namespace mongo
