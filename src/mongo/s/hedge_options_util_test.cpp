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

#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"
#include "mongo/s/hedge_options_util.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

class HedgeOptionsUtilTestFixture : public unittest::Test {
protected:
    /**
     * Set the given server parameters.
     */
    void setParameters(const BSONObj& parameters) {
        const ServerParameter::Map& parameterMap = ServerParameterSet::getGlobal()->getMap();
        BSONObjIterator parameterIterator(parameters);

        while (parameterIterator.more()) {
            BSONElement parameter = parameterIterator.next();
            std::string parameterName = parameter.fieldName();

            ServerParameter::Map::const_iterator foundParameter = parameterMap.find(parameterName);
            uassertStatusOK(foundParameter->second->set(parameter));
        }
    }

    /**
     * Unset the given server parameters by setting them back to the default.
     */
    void unsetParameters(const BSONObj& parameters) {
        const ServerParameter::Map& parameterMap = ServerParameterSet::getGlobal()->getMap();
        BSONObjIterator parameterIterator(parameters);

        while (parameterIterator.more()) {
            BSONElement parameter = parameterIterator.next();
            std::string parameterName = parameter.fieldName();
            const auto defaultParameter = kDefaultParameters[parameterName];
            ASSERT_FALSE(defaultParameter.eoo());

            ServerParameter::Map::const_iterator foundParameter = parameterMap.find(parameterName);
            uassertStatusOK(foundParameter->second->set(defaultParameter));
        }
    }

    /**
     * Sets the given server parameters and sets the ReadPreferenceSetting decoration as given by
     * 'rspObj'on the 'opCtx'. Constructs a RemoteCommandRequestOnAny with 'cmdObjWithoutReadPref'.
     * If 'expectedDelay' is not given, asserts that the RemoteCommandRequestOnAny does not have
     * hedgingOptions set. Otherwise, asserts that hedgingOptions.delay is equal to 'expectedDelay'.
     */
    void checkHedgeOptions(const BSONObj& serverParameters,
                           const BSONObj& cmdObjWithoutReadPref,
                           const BSONObj& rspObj,
                           const boost::optional<Milliseconds> expectedDelay = boost::none) {
        setParameters(serverParameters);

        auto opCtx = _client->makeOperationContext();
        ReadPreferenceSetting::get(opCtx.get()) =
            uassertStatusOK(ReadPreferenceSetting::fromInnerBSON(rspObj));

        auto hedgeOptions = extractHedgeOptions(opCtx.get(), cmdObjWithoutReadPref);
        if (expectedDelay) {
            ASSERT_TRUE(hedgeOptions.has_value());
            ASSERT_EQUALS(hedgeOptions->delay, expectedDelay.get());
        } else {
            ASSERT_FALSE(hedgeOptions.has_value());
        }

        unsetParameters(serverParameters);
    }

    static inline const std::string kCollName = "testColl";

    static inline const std::string kReadHedgingModeFieldName = "readHedgingMode";
    static inline const std::string kMaxTimeMSThresholdForHedgingFieldName =
        "maxTimeMSThresholdForHedging";
    static inline const std::string kHedgingDelayPercentageFieldName = "hedgingDelayPercentage";
    static inline const std::string kDefaultHedgingDelayMSFieldName = "defaultHedgingDelayMS";

    static inline const BSONObj kDefaultParameters =
        BSON(kReadHedgingModeFieldName
             << "on" << kMaxTimeMSThresholdForHedgingFieldName << gMaxTimeMSThresholdForHedging
             << kHedgingDelayPercentageFieldName << gHedgingDelayPercentage
             << kDefaultHedgingDelayMSFieldName << gDefaultHedgingDelayMS);

private:
    ServiceContext::UniqueServiceContext _serviceCtx = ServiceContext::make();
    ServiceContext::UniqueClient _client = _serviceCtx->makeClient("RemoteCommandRequestTest");
};

TEST_F(HedgeOptionsUtilTestFixture, DefaultWithoutMaxTimeMS) {
    const auto parameters = BSONObj();
    const auto cmdObj = BSON("find" << kCollName);
    const auto rspObj = BSON("mode"
                             << "primaryPreferred"
                             << "hedge" << BSONObj());
    const auto expectedDelay = Milliseconds{kDefaultHedgingDelayMSDefault};

    checkHedgeOptions(parameters, cmdObj, rspObj, expectedDelay);
}

TEST_F(HedgeOptionsUtilTestFixture, DefaultWithMaxTimeMS) {
    const auto parameters = BSONObj();
    const auto maxTimeMS = 100;
    const auto cmdObj = BSON("find" << kCollName << "maxTimeMS" << maxTimeMS);
    const auto rspObj = BSON("mode"
                             << "secondary"
                             << "hedge" << BSONObj());
    const auto expectedDelay = Milliseconds{kHedgingDelayPercentageDefault * maxTimeMS / 100};

    checkHedgeOptions(parameters, cmdObj, rspObj, expectedDelay);
}

TEST_F(HedgeOptionsUtilTestFixture, DefaultWithMaxTimeMSAboveThreshold) {
    const auto parameters = BSONObj();
    const auto cmdObj = BSON("find" << kCollName << "maxTimeMS" << 1000000);
    const auto rspObj = BSON("mode"
                             << "secondaryPreferred"
                             << "hedge" << BSONObj());

    checkHedgeOptions(parameters, cmdObj, rspObj);
}

TEST_F(HedgeOptionsUtilTestFixture, DelayDisabled) {
    const auto parameters = BSONObj();
    const auto cmdObj = BSON("find" << kCollName);
    const auto rspObj = BSON("mode"
                             << "nearest"
                             << "hedge" << BSON("delay" << false));
    const auto expectedDelay = Milliseconds{0};

    checkHedgeOptions(parameters, cmdObj, rspObj, expectedDelay);
}

TEST_F(HedgeOptionsUtilTestFixture, HedgingDisabledCompletely) {
    const auto parameters = BSONObj();
    const auto cmdObj = BSON("find" << kCollName);
    const auto rspObj = BSON("mode"
                             << "primaryPreferred"
                             << "hedge" << BSON("enabled" << false << "delay" << false));

    checkHedgeOptions(parameters, cmdObj, rspObj);
}

TEST_F(HedgeOptionsUtilTestFixture, SetMaxTimeMSThreshold) {
    const auto parameters = BSON(kMaxTimeMSThresholdForHedgingFieldName << 1000);
    const auto rspObj = BSON("mode"
                             << "secondary"
                             << "hedge" << BSONObj());

    auto maxTimeMS = 500;
    auto expectedDelay = Milliseconds{kHedgingDelayPercentageDefault * maxTimeMS / 100};
    checkHedgeOptions(
        parameters, BSON("find" << kCollName << "maxTimeMS" << maxTimeMS), rspObj, expectedDelay);

    maxTimeMS = 1200;
    expectedDelay = Milliseconds{0};
    checkHedgeOptions(parameters, BSON("find" << kCollName << "maxTimeMS" << maxTimeMS), rspObj);
}

TEST_F(HedgeOptionsUtilTestFixture, SetHedgingDelayPercentage) {
    const auto parameters = BSON(kHedgingDelayPercentageFieldName << 50);
    const auto rspObj = BSON("mode"
                             << "secondaryPreferred"
                             << "hedge" << BSONObj());

    checkHedgeOptions(
        parameters, BSON("find" << kCollName << "maxTimeMS" << 500), rspObj, Milliseconds{250});
}

TEST_F(HedgeOptionsUtilTestFixture, SetMaxTimeMSThresholdAndHedgingDelayPercentage) {
    const auto parameters = BSON(kMaxTimeMSThresholdForHedgingFieldName
                                 << 1000 << kHedgingDelayPercentageFieldName << 50);
    const auto rspObj = BSON("mode"
                             << "secondaryPreferred"
                             << "hedge" << BSONObj());

    checkHedgeOptions(
        parameters, BSON("find" << kCollName << "maxTimeMS" << 500), rspObj, Milliseconds{250});
    checkHedgeOptions(parameters, BSON("find" << kCollName << "maxTimeMS" << 1200), rspObj);
}

TEST_F(HedgeOptionsUtilTestFixture, SetAll) {
    const auto parameters = BSON(kMaxTimeMSThresholdForHedgingFieldName
                                 << 1000 << kHedgingDelayPercentageFieldName << 50
                                 << kDefaultHedgingDelayMSFieldName << 800);
    const auto rspObj = BSON("mode"
                             << "nearest"
                             << "hedge" << BSONObj());

    checkHedgeOptions(
        parameters, BSON("find" << kCollName << "maxTimeMS" << 500), rspObj, Milliseconds{250});
    checkHedgeOptions(parameters, BSON("find" << kCollName), rspObj, Milliseconds{800});
}

TEST_F(HedgeOptionsUtilTestFixture, ReadHedgingModeOff) {
    const auto parameters =
        BSON(kReadHedgingModeFieldName << "off" << kMaxTimeMSThresholdForHedgingFieldName << 1000
                                       << kHedgingDelayPercentageFieldName << 50
                                       << kDefaultHedgingDelayMSFieldName << 800);
    const auto rspObj = BSON("mode"
                             << "nearest"
                             << "hedge" << BSONObj());

    checkHedgeOptions(parameters, BSON("find" << kCollName << "maxTimeMS" << 500), rspObj);
    checkHedgeOptions(parameters, BSON("find" << kCollName), rspObj);
}

}  // namespace
}  // namespace mongo
