// @file task.cpp

/*    Copyright 2009 10gen Inc.
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

#include "mongo/util/concurrency/task.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace task {

Task::Task() : BackgroundJob(true /* deleteSelf */) {
    n = 0;
    repeat = 0;
}

void Task::halt() {
    repeat = 0;
}

void Task::setUp() {}

void Task::run() {
    verify(n == 0);

    setUp();

    while (1) {
        n++;
        try {
            doWork();
        } catch (...) {
        }
        sleepmillis(repeat);
        if (inShutdown())
            break;
        if (repeat == 0)
            break;
    }
}

void Task::begin() {
    go();
}

void fork(Task* t) {
    t->begin();
}

void repeat(Task* t, unsigned millis) {
    t->repeat = millis;
    t->begin();
}
}
}
