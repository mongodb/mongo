/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <string>

#include "mongo/db/field_parser.h"
#include "mongo/db/range_deleter.h"
#include "mongo/db/range_deleter_mock_env.h"
#include "mongo/db/range_deleter_stats.h"
#include "mongo/unittest/unittest.h"

namespace {

    using boost::bind;
    using std::string;

    using mongo::BSONObj;
    using mongo::CursorId;
    using mongo::FieldParser;
    using mongo::Notification;
    using mongo::RangeDeleter;
    using mongo::RangeDeleterMockEnv;
    using mongo::RangeDeleterStats;

    TEST(NoDeletes, InitialState) {
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);

        const BSONObj stats(deleter.getStats()->toBSON());

        int totalCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::TotalDeletesField,
                                         &totalCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, totalCount);

        int pendingCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::PendingDeletesField,
                                         &pendingCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, pendingCount);

        int inProgressCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::InProgressDeletesField,
                                         &inProgressCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, inProgressCount);

        deleter.stopWorkers();
    }

    TEST(QueuedDeletes, NotReady) {
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);

        const string ns("test.user");
        deleter.startWorkers();

        // Set cursors on NS so deletes cannot be processed immediately.
        env->addCursorId(ns, 50);

        string errMsg;
        Notification notifyDone;
        ASSERT_TRUE(deleter.queueDelete(ns,
                                        BSON("x" << 0),
                                        BSON("x" << 10),
                                        BSON("x" << 1),
                                        true,
                                        &notifyDone,
                                        &errMsg));
        ASSERT_TRUE(errMsg.empty());

        env->waitForNthGetCursor(1u);

        const BSONObj stats(deleter.getStats()->toBSON());

        int totalCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::TotalDeletesField,
                                         &totalCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(1, totalCount);

        int pendingCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::PendingDeletesField,
                                         &pendingCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(1, pendingCount);

        int inProgressCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::InProgressDeletesField,
                                         &inProgressCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, inProgressCount);

        deleter.stopWorkers();
    }

    TEST(QueuedDeletes, InProgress) {
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);

        const string ns("test.user");
        deleter.startWorkers();

        env->pauseDeletes();

        Notification deleteDone;
        string errMsg;
        ASSERT_TRUE(deleter.queueDelete(ns,
                                        BSON("x" << 0),
                                        BSON("x" << 10),
                                        BSON("x" << 1),
                                        true,
                                        &deleteDone,
                                        &errMsg));
        ASSERT_TRUE(errMsg.empty());

        env->waitForNthPausedDelete(1u);

        const BSONObj stats(deleter.getStats()->toBSON());

        int totalCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::TotalDeletesField,
                                         &totalCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(1, totalCount);

        int inProgressCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::InProgressDeletesField,
                                         &inProgressCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(1, inProgressCount);

        int pendingCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::PendingDeletesField,
                                         &pendingCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, pendingCount);

        env->resumeOneDelete();
        deleteDone.waitToBeNotified();

        deleter.stopWorkers();
    }

    TEST(QueuedDeletes, AfterDelete) {
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);

        const string ns("test.user");
        deleter.startWorkers();

        string errMsg;
        Notification notifyDone;
        ASSERT_TRUE(deleter.queueDelete(ns,
                                        BSON("x" << 0),
                                        BSON("x" << 10),
                                        BSON("x" << 1),
                                        true,
                                        &notifyDone,
                                        &errMsg));
        ASSERT_TRUE(errMsg.empty());

        notifyDone.waitToBeNotified();

        const BSONObj stats(deleter.getStats()->toBSON());

        int totalCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::TotalDeletesField,
                                         &totalCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, totalCount);

        int pendingCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::PendingDeletesField,
                                         &pendingCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, pendingCount);

        int inProgressCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::InProgressDeletesField,
                                         &inProgressCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, inProgressCount);

        deleter.stopWorkers();
    }

    TEST(ImmediateDeletes, NotReady) {
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);

        const string ns("test.user");

        // Set cursors on NS so deletes cannot be processed immediately.
        env->addCursorId(ns, 50);

        string errMsg;
        boost::thread deleterThread = boost::thread(boost::bind(&RangeDeleter::deleteNow,
                                                                &deleter,
                                                                ns,
                                                                BSON("x" << 0),
                                                                BSON("x" << 10),
                                                                BSON("x" << 1),
                                                                true,
                                                                &errMsg));
        env->waitForNthGetCursor(1u);

        const BSONObj stats(deleter.getStats()->toBSON());

        int totalCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::TotalDeletesField,
                                         &totalCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(1, totalCount);

        // Note: immediate deletes has no pending state, it goes directly to inProgress
        // even while waiting for cursors.
        int pendingCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::PendingDeletesField,
                                         &pendingCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, pendingCount);

        int inProgressCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::InProgressDeletesField,
                                         &inProgressCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(1, inProgressCount);

        env->removeCursorId(ns, 50);
        deleterThread.join();

        deleter.stopWorkers();
    }

    TEST(ImmediateDeletes, InProgress) {
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);

        const string ns("test.user");
        env->pauseDeletes();

        string errMsg;
        boost::thread deleterThread = boost::thread(boost::bind(&RangeDeleter::deleteNow,
                                                                &deleter,
                                                                ns,
                                                                BSON("x" << 0),
                                                                BSON("x" << 10),
                                                                BSON("x" << 1),
                                                                true,
                                                                &errMsg));

        env->waitForNthPausedDelete(1u);

        const BSONObj stats(deleter.getStats()->toBSON());

        int totalCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::TotalDeletesField,
                                         &totalCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(1, totalCount);

        // There is only one worker thread so you can't have inProgress > 1
        int inProgressCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::InProgressDeletesField,
                                         &inProgressCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(1, inProgressCount);

        // The rest should still be in pending since there is only one worker.
        int pendingCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::PendingDeletesField,
                                         &pendingCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, pendingCount);

        env->resumeOneDelete();
        deleterThread.join();

        deleter.stopWorkers();
    }

    TEST(ImmediateDeletes, AfterDelete) {
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);

        const string ns("test.user");
        string errMsg;
        ASSERT_TRUE(deleter.deleteNow(ns, BSON("x" << 0), BSON("x" << 10),
                                      BSON("x" << 1), true, &errMsg));

        const BSONObj stats(deleter.getStats()->toBSON());

        int totalCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::TotalDeletesField,
                                         &totalCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, totalCount);

        int pendingCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::PendingDeletesField,
                                         &pendingCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, pendingCount);

        int inProgressCount = 0;
        ASSERT_TRUE(FieldParser::extract(stats, RangeDeleterStats::InProgressDeletesField,
                                         &inProgressCount, NULL /* don't care errMsg */));
        ASSERT_EQUALS(0, inProgressCount);

        deleter.stopWorkers();
    }

} // unnamed namespace
