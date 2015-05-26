/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_settings.h"

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace {

    using namespace mongo;

    TEST(SettingsType, MissingKey) {
        BSONObj objNoKey = BSONObj();
        StatusWith<SettingsType> result = SettingsType::fromBSON(objNoKey);
        ASSERT_FALSE(result.isOK());
        ASSERT_EQUALS(result.getStatus(), ErrorCodes::NoSuchKey);
    }

    TEST(SettingsType, ChunkSize) {
        BSONObj objChunkSizeZero = BSON(SettingsType::key(SettingsType::ChunkSizeDocKey) <<
                                        SettingsType::chunkSize(0));
        StatusWith<SettingsType> result = SettingsType::fromBSON(objChunkSizeZero);
        ASSERT(result.isOK());
        SettingsType settings = result.getValue();
        ASSERT_EQUALS(settings.getChunkSize(), 0);
        Status validationStatus = settings.validate();
        ASSERT_FALSE(validationStatus.isOK());
        ASSERT_EQUALS(validationStatus, ErrorCodes::BadValue);
    }

    TEST(SettingsType, UnsupportedSetting) {
        BSONObj objBadSetting = BSON(SettingsType::key("badsetting"));
        StatusWith<SettingsType> result = SettingsType::fromBSON(objBadSetting);
        ASSERT(result.isOK());
        SettingsType settings = result.getValue();
        Status validationStatus = settings.validate();
        ASSERT_FALSE(validationStatus.isOK());
        ASSERT_EQUALS(validationStatus, ErrorCodes::UnsupportedFormat);
    }

    TEST(SettingsType, InvalidBalancerWindow) {
        BSONObj objBalancerBadKeys = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                                          SettingsType::balancerActiveWindow(BSON("begin" <<
                                                                                  "23:00" <<
                                                                                  "end" << 
                                                                                  "6:00" )));
        StatusWith<SettingsType> result = SettingsType::fromBSON(objBalancerBadKeys);
        ASSERT_FALSE(result.isOK());

        BSONObj objBalancerBadTimes = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                                           SettingsType::balancerActiveWindow(BSON("start" <<
                                                                                   "23" <<
                                                                                   "stop" <<
                                                                                   "6" )));
        result = SettingsType::fromBSON(objBalancerBadTimes);
        ASSERT_FALSE(result.isOK());
    }

    TEST(SettingsType, ValidValues) {
        BSONObj objChunkSize = BSON(SettingsType::key(SettingsType::ChunkSizeDocKey) <<
                                    SettingsType::chunkSize(1));
        StatusWith<SettingsType> result = SettingsType::fromBSON(objChunkSize);
        SettingsType settings = result.getValue();
        ASSERT(result.isOK());
        Status validationStatus = settings.validate();
        ASSERT(validationStatus.isOK());
        ASSERT_EQUALS(settings.getKey(), SettingsType::ChunkSizeDocKey);
        ASSERT_EQUALS(settings.getChunkSize(), 1);

        BSONObj objBalancer = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                                   SettingsType::balancerStopped(true) <<
                                   SettingsType::balancerActiveWindow(BSON("start" << "23:00" <<
                                                                           "stop" << "6:00" )) <<
                                   SettingsType::migrationWriteConcern(BSON("w" << 2)));
        result = SettingsType::fromBSON(objBalancer);
        settings = result.getValue();
        ASSERT(result.isOK());
        validationStatus = settings.validate();
        ASSERT(validationStatus.isOK());
        ASSERT_EQUALS(settings.getKey(), SettingsType::BalancerDocKey);
        ASSERT_EQUALS(settings.getBalancerStopped(), true);

        WriteConcernOptions wc;
        wc.parse(BSON("w" << 2));
        ASSERT_EQUALS(settings.getMigrationWriteConcern().toBSON(), wc.toBSON());
    }

    TEST(SettingsType, ValidWithDeprecatedThrottle) {
        BSONObj objChunkSize = BSON(SettingsType::key(SettingsType::ChunkSizeDocKey) <<
                                    SettingsType::chunkSize(1));
        StatusWith<SettingsType> result = SettingsType::fromBSON(objChunkSize);
        ASSERT(result.isOK());
        SettingsType settings = result.getValue();
        ASSERT_EQUALS(settings.getKey(), SettingsType::ChunkSizeDocKey);
        ASSERT_EQUALS(settings.getChunkSize(), 1);

        BSONObj objBalancer = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                                   SettingsType::deprecated_secondaryThrottle(true));
        result = SettingsType::fromBSON(objBalancer);
        ASSERT_EQUALS(result.getStatus(), Status::OK());
        settings = result.getValue();
        ASSERT_EQUALS(settings.getKey(), SettingsType::BalancerDocKey);
        ASSERT(settings.getSecondaryThrottle());
    }

    TEST(SettingsType, BadType) {
        BSONObj badTypeObj = BSON(SettingsType::key() << 0);
        StatusWith<SettingsType> result = SettingsType::fromBSON(badTypeObj);
        ASSERT_FALSE(result.isOK());
    }

    TEST(SettingsType, BalancingWindow) {
        // T0 < T1 < now < T2 < T3 and Error
        const std::string T0 = "9:00";
        const std::string T1 = "11:00";
        boost::posix_time::ptime now(currentDate(),
                                     boost::posix_time::hours(13) +
                                        boost::posix_time::minutes(48));
        const std::string T2 = "17:00";
        const std::string T3 = "21:30";
        const std::string E = "28:35";

        // closed in the past
        BSONObj w1 = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                          SettingsType::balancerActiveWindow(BSON("start" << T0 << "stop" << T1)));
        StatusWith<SettingsType> result = SettingsType::fromBSON(w1);
        ASSERT(result.isOK());
        ASSERT_FALSE(result.getValue().inBalancingWindow(now));

        // not opened until the future
        BSONObj w2 = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                          SettingsType::balancerActiveWindow(BSON("start" << T2 << "stop" << T3)));
        result = SettingsType::fromBSON(w2);
        ASSERT(result.isOK());
        ASSERT_FALSE(result.getValue().inBalancingWindow(now));

        // open now
        BSONObj w3 = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                          SettingsType::balancerActiveWindow(BSON("start" << T1 << "stop" << T2)));
        result = SettingsType::fromBSON(w3);
        ASSERT(result.isOK());
        ASSERT(result.getValue().inBalancingWindow(now));

        // open since last day
        BSONObj w4 = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                          SettingsType::balancerActiveWindow(BSON("start" << T3 << "stop" << T2)));
        result = SettingsType::fromBSON(w4);
        ASSERT(result.isOK());
        ASSERT(result.getValue().inBalancingWindow(now));

        // bad input should not stop the balancer

        // empty window
        BSONObj w5 = BSON(SettingsType::key(SettingsType::BalancerDocKey));
        result = SettingsType::fromBSON(w5);
        ASSERT(result.isOK());
        ASSERT(result.getValue().inBalancingWindow(now));

        // missing stop
        BSONObj w6 = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                          SettingsType::balancerActiveWindow(BSON("start" << 1)));
        result = SettingsType::fromBSON(w6);
        ASSERT_FALSE(result.isOK());

        // missing start
        BSONObj w7 = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                          SettingsType::balancerActiveWindow(BSON("stop" << 1)));
        result = SettingsType::fromBSON(w7);
        ASSERT_FALSE(result.isOK());

        // active window marker missing
        BSONObj w8 = BSON(SettingsType::key(SettingsType::BalancerDocKey) <<
                          "wrongMarker" << BSON("start" << 1 << "stop" << 1));
        result = SettingsType::fromBSON(w8);
        ASSERT(result.isOK());
        ASSERT(result.getValue().inBalancingWindow(now));

        // garbage in window
        BSONObj w9 = BSON(SettingsType::balancerActiveWindow(BSON("start" << T3 << "stop" << E)));
        result = SettingsType::fromBSON(w9);
        ASSERT_FALSE(result.isOK());
    }

} // unnamed namespace
