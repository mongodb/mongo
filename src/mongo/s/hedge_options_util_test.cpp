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
    void setUp() {
        // Reset all the hedging server parameters.
        setParameters(kDefaultParameters);
    }

    /**
     * Set the given server parameters.
     */
    void setParameters(const BSONObj& parameters) {
        auto* paramSet = ServerParameterSet::getNodeParameterSet();
        BSONObjIterator parameterIterator(parameters);
        while (parameterIterator.more()) {
            BSONElement parameter = parameterIterator.next();
            uassertStatusOK(paramSet->get(parameter.fieldName())->set(parameter, boost::none));
        }
    }

    /**
     * Sets the given server parameters and creates ReadPreferenceSetting from 'rspObj' and extracts
     * HedgeOptions from it. If 'hedge' is true, asserts that the resulting HedgeOptions is not
     * empty, otherwise asserts that it is empty. Resets the server parameters to the defaults
     * before returning.
     */
    void checkHedgeOptions(const BSONObj& serverParameters,
                           const BSONObj& cmdObj,
                           const BSONObj& rspObj,
                           const bool hedge,
                           const int maxTimeMSForHedgedReads = kMaxTimeMSForHedgedReadsDefault) {
        setParameters(serverParameters);

        auto readPref = uassertStatusOK(ReadPreferenceSetting::fromInnerBSON(rspObj));
        executor::RemoteCommandRequest::Options options;
        extractHedgeOptions(cmdObj, readPref, options);

        ASSERT_EQ(hedge, options.isHedgeEnabled);
        if (hedge) {
            ASSERT_EQ(options.maxTimeMSForHedgedReads, maxTimeMSForHedgedReads);
        }
    }

    static inline const std::string kCollName = "testColl";
    static inline const StringData mapJavascript = "map!"_sd;
    static inline const StringData reduceJavascript = "reduce!"_sd;

    static inline const std::string kReadHedgingModeFieldName = "readHedgingMode";
    static inline const std::string kMaxTimeMSForHedgedReadsFieldName = "maxTimeMSForHedgedReads";
    static inline const int kMaxTimeMSForHedgedReadsDefault = 10;

    static inline const BSONObj kDefaultParameters =
        BSON(kReadHedgingModeFieldName << "on" << kMaxTimeMSForHedgedReadsFieldName
                                       << kMaxTimeMSForHedgedReadsDefault);

private:
    ServiceContext::UniqueServiceContext _serviceCtx = ServiceContext::make();
    ServiceContext::UniqueClient _client = _serviceCtx->makeClient("RemoteCommandRequestTest");
};

TEST_F(HedgeOptionsUtilTestFixture, ExplicitOperationHedging) {
    const auto parameters = BSONObj();
    const auto cmdObj = BSON("find" << kCollName);
    const auto rspObj = BSON("mode"
                             << "primaryPreferred"
                             << "hedge" << BSONObj());

    checkHedgeOptions(parameters, cmdObj, rspObj, true);
}

TEST_F(HedgeOptionsUtilTestFixture, ImplicitOperationHedging) {
    const auto parameters = BSONObj();
    const auto cmdObj = BSON("find" << kCollName);
    const auto rspObj = BSON("mode"
                             << "nearest");

    checkHedgeOptions(parameters, cmdObj, rspObj, true);
}

TEST_F(HedgeOptionsUtilTestFixture, DenylistAggregate) {
    const auto parameters = BSONObj();
    const auto cmdObj =
        BSON("aggregate" << kCollName << "pipeline" << BSONObj() << "cursor" << BSONObj());
    const auto rspObj = BSON("mode"
                             << "nearest"
                             << "hedge" << BSONObj());

    checkHedgeOptions(parameters, cmdObj, rspObj, false);
}

TEST_F(HedgeOptionsUtilTestFixture, DenylistMapReduce) {
    const auto parameters = BSONObj();
    const auto rspObj = BSON("mode"
                             << "nearest"
                             << "hedge" << BSONObj());

    {
        const auto cmdObj = BSON("mapreduce"
                                 << "sourceColl"
                                 << "map" << mapJavascript << "reduce" << reduceJavascript << "out"
                                 << "targetColl"
                                 << "$db"
                                 << "db");
        checkHedgeOptions(parameters, cmdObj, rspObj, false);
    }

    {
        const auto cmdObj = BSON("mapReduce"
                                 << "sourceColl"
                                 << "map" << mapJavascript << "reduce" << reduceJavascript << "out"
                                 << "targetColl"
                                 << "$db"
                                 << "db");
        checkHedgeOptions(parameters, cmdObj, rspObj, false);
    }
}

TEST_F(HedgeOptionsUtilTestFixture, OperationHedgingDisabled) {
    const auto parameters = BSONObj();
    const auto cmdObj = BSON("find" << kCollName);
    const auto rspObj = BSON("mode"
                             << "nearest"
                             << "hedge" << BSON("enabled" << false));

    checkHedgeOptions(parameters, cmdObj, rspObj, false);
}

TEST_F(HedgeOptionsUtilTestFixture, ReadHedgingModeOff) {
    const auto parameters = BSON(kReadHedgingModeFieldName << "off");
    const auto cmdObj = BSON("find" << kCollName);
    const auto rspObj = BSON("mode"
                             << "nearest"
                             << "hedge" << BSONObj());

    checkHedgeOptions(parameters, cmdObj, rspObj, false);
}

TEST_F(HedgeOptionsUtilTestFixture, MaxTimeMSForHedgedReads) {
    const auto parameters =
        BSON(kReadHedgingModeFieldName << "on" << kMaxTimeMSForHedgedReadsFieldName << 100);
    const auto cmdObj = BSON("find" << kCollName);
    const auto rspObj = BSON("mode"
                             << "nearest"
                             << "hedge" << BSONObj());

    checkHedgeOptions(parameters, cmdObj, rspObj, true, 100);
}

}  // namespace
}  // namespace mongo
