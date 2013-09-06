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

#include "mongo/s/type_settings.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::string;
    using mongo::BSONObj;
    using mongo::SettingsType;

    TEST(Validity, MissingFields) {
        SettingsType settings;
        BSONObj objNoKey = BSONObj();
        string errMsg;
        ASSERT(settings.parseBSON(objNoKey, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(settings.isValid(NULL));

        BSONObj objChunksizeNoValue = BSON(SettingsType::key("chunksize"));
        ASSERT(settings.parseBSON(objChunksizeNoValue, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(settings.isValid(NULL));
    }

    TEST(Validity, UnsupportedSetting) {
        SettingsType settings;
        BSONObj objBadSetting = BSON(SettingsType::key("badsetting"));
        string errMsg;
        ASSERT(settings.parseBSON(objBadSetting, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(settings.isValid(NULL));
    }

    TEST(Validity, InvalidBalancerWindow) {
        SettingsType settings;
        BSONObj objBalancerBadKeys = BSON(SettingsType::key("balancer") <<
                                          SettingsType::balancerActiveWindow(BSON("begin" <<
                                                                                  "23:00" <<
                                                                                  "end" << 
                                                                                  "6:00" )));
        string errMsg;
        ASSERT(settings.parseBSON(objBalancerBadKeys, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(settings.isValid(NULL));
        BSONObj objBalancerBadTimes = BSON(SettingsType::key("balancer") <<
                                           SettingsType::balancerActiveWindow(BSON("start" <<
                                                                                   "23" <<
                                                                                   "stop" <<
                                                                                   "6" )));
        ASSERT(settings.parseBSON(objBalancerBadTimes, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(settings.isValid(NULL));
    }

    TEST(Validity, Valid) {
        SettingsType settings;
        BSONObj objChunksize = BSON(SettingsType::key("chunksize") <<
                                    SettingsType::chunksize(1));
        string errMsg;
        ASSERT(settings.parseBSON(objChunksize, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(settings.isValid(NULL));
        ASSERT_EQUALS(settings.getKey(), "chunksize");
        ASSERT_EQUALS(settings.getChunksize(), 1);

        BSONObj objBalancer = BSON(SettingsType::key("balancer") <<
                           SettingsType::balancerStopped(true) <<
                           SettingsType::balancerActiveWindow(BSON("start" << "23:00" <<
                                                                   "stop" << "6:00" )) <<
                           SettingsType::shortBalancerSleep(true) <<
                           SettingsType::secondaryThrottle(true));
        ASSERT(settings.parseBSON(objBalancer, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(settings.isValid(NULL));
        ASSERT_EQUALS(settings.getKey(), "balancer");
        ASSERT_EQUALS(settings.getBalancerStopped(), true);
        ASSERT_EQUALS(settings.getBalancerActiveWindow(), BSON("start" << "23:00" <<
                                                               "stop" << "6:00" ));
        ASSERT_EQUALS(settings.getShortBalancerSleep(), true);
        ASSERT_EQUALS(settings.getSecondaryThrottle(), true);
    }

    TEST(Validity, BadType) {
        SettingsType settings;
        BSONObj obj = BSON(SettingsType::key() << 0);
        string errMsg;
        ASSERT((!settings.parseBSON(obj, &errMsg)) && (errMsg != ""));
    }

} // unnamed namespace
