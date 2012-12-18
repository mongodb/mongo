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
 */

#include "mongo/s/type_settings.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::SettingsType;

    TEST(Validity, MissingFields) {
        SettingsType settings;
        BSONObj objNoKey = BSONObj();
        settings.parseBSON(objNoKey);
        ASSERT_FALSE(settings.isValid(NULL));

        BSONObj objChunksizeNoValue = BSON(SettingsType::key("chunksize"));
        settings.parseBSON(objChunksizeNoValue);
        ASSERT_FALSE(settings.isValid(NULL));
    }

    TEST(Validity, UnsupportedSetting) {
        SettingsType settings;
        BSONObj objBadSetting = BSON(SettingsType::key("badsetting"));
        settings.parseBSON(objBadSetting);
        ASSERT_FALSE(settings.isValid(NULL));
    }

    TEST(Validity, InvalidBalancerWindow) {
        SettingsType settings;
        BSONObj objBalancerBadKeys = BSON(SettingsType::key("balancer") <<
                                          SettingsType::balancerActiveWindow(BSON("begin" <<
                                                                                  "23:00" <<
                                                                                  "end" << 
                                                                                  "6:00" )));
        settings.parseBSON(objBalancerBadKeys);
        ASSERT_FALSE(settings.isValid(NULL));
        BSONObj objBalancerBadTimes = BSON(SettingsType::key("balancer") <<
                                           SettingsType::balancerActiveWindow(BSON("start" <<
                                                                                   "23" <<
                                                                                   "stop" <<
                                                                                   "6" )));
        settings.parseBSON(objBalancerBadTimes);
        ASSERT_FALSE(settings.isValid(NULL));
    }

    TEST(Validity, Valid) {
        SettingsType settings;
        BSONObj objChunksize = BSON(SettingsType::key("chunksize") <<
                                    SettingsType::chunksize(1));
        settings.parseBSON(objChunksize);
        ASSERT_TRUE(settings.isValid(NULL));
        ASSERT_EQUALS(settings.getKey(), "chunksize");
        ASSERT_EQUALS(settings.getChunksize(), 1);

        BSONObj objBalancer = BSON(SettingsType::key("balancer") <<
                           SettingsType::balancerStopped(true) <<
                           SettingsType::balancerActiveWindow(BSON("start" << "23:00" <<
                                                                   "stop" << "6:00" )) <<
                           SettingsType::shortBalancerSleep(true) <<
                           SettingsType::secondaryThrottle(true));
        settings.parseBSON(objBalancer);
        ASSERT_TRUE(settings.isValid(NULL));
        ASSERT_EQUALS(settings.getKey(), "balancer");
        ASSERT_EQUALS(settings.getBalancerStopped(), true);
        ASSERT_EQUALS(settings.getBalancerActiveWindow(), BSON("start" << "23:00" <<
                                                               "stop" << "6:00" ));
        ASSERT_EQUALS(settings.getShortBalancerSleep(), true);
        ASSERT_EQUALS(settings.getSecondaryThrottle(), true);
    }

} // unnamed namespace
