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

#include <boost/thread/thread.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"
#include "mongo/unittest/unittest.h"

using mongo::FailPoint;

namespace mongo_test {
    TEST(FailPoint, InitialState) {
        FailPoint failPoint;
        ASSERT_FALSE(failPoint.shouldFail());
        ASSERT(failPoint.getData().isEmpty());
        ASSERT_FALSE(failPoint.shouldFail());
    }

    TEST(FailPoint, AlwaysOn) {
        FailPoint failPoint;
        failPoint.setMode(FailPoint::alwaysOn);
        ASSERT(failPoint.shouldFail());
        ASSERT(failPoint.getData().isEmpty());

        for (size_t x = 0; x < 50; x++) {
            ASSERT(failPoint.shouldFail());
        }
    }

    TEST(FailPoint, NTimes) {
        FailPoint failPoint;
        failPoint.setMode(FailPoint::nTimes, 4);
        ASSERT(failPoint.shouldFail());
        ASSERT(failPoint.shouldFail());
        ASSERT(failPoint.shouldFail());
        ASSERT(failPoint.shouldFail());

        for (size_t x = 0; x < 50; x++) {
            ASSERT_FALSE(failPoint.shouldFail());
        }
    }

    TEST(FailPoint, BlockOff) {
        FailPoint failPoint;
        bool called = false;

        MONGO_FAIL_POINT_BLOCK(failPoint) {
            called = true;
        }

        ASSERT_FALSE(called);
    }

    TEST(FailPoint, BlockAlwaysOn) {
        FailPoint failPoint;
        failPoint.setMode(FailPoint::alwaysOn);
        bool called = false;

        MONGO_FAIL_POINT_BLOCK(failPoint) {
            called = true;
        }

        ASSERT(called);
    }

    TEST(FailPoint, BlockNTimes) {
        FailPoint failPoint;
        failPoint.setMode(FailPoint::nTimes, 1);
        size_t counter = 0;

        for (size_t x = 0; x < 10; x++) {
            MONGO_FAIL_POINT_BLOCK(failPoint) {
                counter++;
            }
        }

        ASSERT_EQUALS(1U, counter);
    }

    TEST(FailPoint, BlockWithException) {
        FailPoint failPoint;
        failPoint.setMode(FailPoint::alwaysOn);
        bool threw = false;

        try {
            MONGO_FAIL_POINT_BLOCK(failPoint) {
                throw std::logic_error("BlockWithException threw");
            }
        }
        catch (const std::logic_error &) {
            threw = true;
        }

        ASSERT(threw);
        // This will get into an infinite loop if reference counter was not
        // properly decremented
        failPoint.setMode(FailPoint::off);
    }

    TEST(FailPoint, SetGetParam) {
        FailPoint failPoint;
        failPoint.setMode(FailPoint::alwaysOn, 0, BSON("x" << 20));

        MONGO_FAIL_POINT_BLOCK(failPoint) {
            ASSERT_EQUALS(20, failPoint.getData()["x"].numberInt());
        }
    }

    TEST(FailPoint, SetInvalidMode) {
        FailPoint failPoint;

        ASSERT_THROWS(failPoint.setMode(static_cast<FailPoint::Mode>(9999)),
                      mongo::UserException);
        ASSERT_FALSE(failPoint.shouldFail());

        ASSERT_THROWS(failPoint.setMode(static_cast<FailPoint::Mode>(-1)),
                      mongo::UserException);
        ASSERT_FALSE(failPoint.shouldFail());
    }

    class FailPointStress: public mongo::unittest::Test {
    public:
        FailPointStress(): _tasks(NULL) {
        }

        void setUp() {
            _fp.setMode(FailPoint::alwaysOn, 0, BSON("a" << 44));
        }

        void tearDown() {
            // Note: This can loop indefinitely if reference counter was off
            _fp.setMode(FailPoint::off, 0, BSON("a" << 66));
        }

        void startTest() {
            verify(_tasks == NULL);

            _tasks = new boost::thread_group();
            _tasks->add_thread(new boost::thread(blockTask, &_fp));
            _tasks->add_thread(new boost::thread(blockWithExceptionTask, &_fp));
            _tasks->add_thread(new boost::thread(simpleTask, &_fp));
            _tasks->add_thread(new boost::thread(flipTask, &_fp));
        }

        void stopTest() {
            _tasks->interrupt_all();
            _tasks->join_all();
            delete _tasks;
            _tasks = NULL;
        }

    private:
        static void blockTask(FailPoint* failPoint) {
            while (true) {
                MONGO_FAIL_POINT_BLOCK((*failPoint)) {
                    const mongo::BSONObj& data = failPoint->getData();
                    ASSERT_EQUALS(44, data["a"].numberInt());
                }

                boost::this_thread::interruption_point();
            }
        }

        static void blockWithExceptionTask(FailPoint* failPoint) {
            while (true) {
                try {
                    MONGO_FAIL_POINT_BLOCK((*failPoint)) {
                        const mongo::BSONObj& data = failPoint->getData();
                        ASSERT_EQUALS(44, data["a"].numberInt());
                        throw std::logic_error("blockWithExceptionTask threw");
                    }
                }
                catch (const std::logic_error&) {
                }

                boost::this_thread::interruption_point();
            }
        }

        static void simpleTask(FailPoint* failPoint) {
            while (true) {
                if (MONGO_FAIL_POINT((*failPoint))) {
                    const mongo::BSONObj& data = failPoint->getData();
                    ASSERT_EQUALS(44, data["a"].numberInt());
                }

                boost::this_thread::interruption_point();
            }
        }

        static void flipTask(FailPoint* failPoint) {
            while (true) {
                if(failPoint->shouldFail()) {
                    failPoint->setMode(FailPoint::off, 0);
                }
                else {
                    failPoint->setMode(FailPoint::alwaysOn, 0, BSON("a" << 44));
                }

                boost::this_thread::interruption_point();
            }
        }

        FailPoint _fp;
        boost::thread_group* _tasks;
    };

    TEST_F(FailPointStress, Basic) {
        startTest();
        mongo::sleepsecs(120);
        stopTest();
    }
}
