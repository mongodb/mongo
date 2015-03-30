/*    Copyright 2012 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/s/grid.h"
#include "mongo/s/type_settings.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::string;

namespace {

    TEST(BalancingWindow, Test) {
        // T0 < T1 < now < T2 < T3 and Error
        const string T0 = "9:00";
        const string T1 = "11:00";
        boost::posix_time::ptime now(currentDate(),
                                     boost::posix_time::hours(13) +
                                        boost::posix_time::minutes(48));
        const string T2 = "17:00";
        const string T3 = "21:30";
        const string E = "28:35";

        // closed in the past
        BSONObj w1 = BSON(SettingsType::balancerActiveWindow(BSON("start" << T0 << "stop" << T1)));

        // not opened until the future
        BSONObj w2 = BSON(SettingsType::balancerActiveWindow(BSON("start" << T2 << "stop" << T3)));

        // open now
        BSONObj w3 = BSON(SettingsType::balancerActiveWindow(BSON("start" << T1 << "stop" << T2)));

        // open since last day
        BSONObj w4 = BSON(SettingsType::balancerActiveWindow(BSON("start" << T3 << "stop" << T2)));

        ASSERT(!Grid::_inBalancingWindow(w1, now));
        ASSERT(!Grid::_inBalancingWindow(w2, now));
        ASSERT(Grid::_inBalancingWindow(w3, now));
        ASSERT(Grid::_inBalancingWindow(w4, now));

        // bad input should not stop the balancer

        // empty window
        BSONObj w5;

        // missing stop
        BSONObj w6 = BSON(SettingsType::balancerActiveWindow(BSON("start" << 1)));

        // missing start
        BSONObj w7 = BSON(SettingsType::balancerActiveWindow(BSON("stop" << 1)));

        // active window marker missing
        BSONObj w8 = BSON("wrongMarker" << 1 << "start" << 1 << "stop" << 1);

        // garbage in window
        BSONObj w9 = BSON(SettingsType::balancerActiveWindow(BSON("start" << T3 << "stop" << E)));

        ASSERT(Grid::_inBalancingWindow(w5, now));
        ASSERT(Grid::_inBalancingWindow(w6, now));
        ASSERT(Grid::_inBalancingWindow(w7, now));
        ASSERT(Grid::_inBalancingWindow(w8, now));
        ASSERT(Grid::_inBalancingWindow(w9, now));
    }

} // namespace
} // namespace mongo
