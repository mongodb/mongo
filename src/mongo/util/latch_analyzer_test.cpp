/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/source_location.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {
namespace {

using Level = HierarchicalAcquisitionLevel;

class LatchAnalyzerTest : public ServiceContextTest {};

DEATH_TEST_F(LatchAnalyzerTest, AddInvalidWasAbsent, "Fatal assertion 31360") {

    Mutex lowerLevel = MONGO_MAKE_LATCH(
        Level(1), (SourceLocationHolder)MONGO_SOURCE_LOCATION(), "AddInvalidWasAbsent::lowerLevel");
    lowerLevel.lock();
    Mutex higherLevel = MONGO_MAKE_LATCH(Level(2),
                                         (SourceLocationHolder)MONGO_SOURCE_LOCATION(),
                                         "AddInvalidWasAbsent::higherLevel");
    higherLevel.lock();
}

DEATH_TEST_F(LatchAnalyzerTest, AddInvalidWasPresent, "Fatal assertion 31360") {
    Mutex m1 = MONGO_MAKE_LATCH(
        Level(1), (SourceLocationHolder)MONGO_SOURCE_LOCATION(), "AddInvalidWasPresent::m1");
    Mutex m2 = MONGO_MAKE_LATCH(
        Level(1), (SourceLocationHolder)MONGO_SOURCE_LOCATION(), "AddInvalidWasPresent::m2");
    m1.lock();
    m2.lock();
}

DEATH_TEST_F(LatchAnalyzerTest, RemoveInvalidWasAbsent, "Fatal assertion 31361") {
    Mutex m = MONGO_MAKE_LATCH(
        Level(1), (SourceLocationHolder)MONGO_SOURCE_LOCATION(), "RemoveInvalidWasAbsent::m");
    m.unlock();
    m.unlock();
}

DEATH_TEST_F(LatchAnalyzerTest, RemoveInvalidWasPresent, "Fatal assertion 31361") {
    Mutex higherLevel = MONGO_MAKE_LATCH(Level(2),
                                         (SourceLocationHolder)MONGO_SOURCE_LOCATION(),
                                         "RemoveInvalidWasPresent::higherLevel");
    higherLevel.lock();
    Mutex lowerLevel = MONGO_MAKE_LATCH(Level(1),
                                        (SourceLocationHolder)MONGO_SOURCE_LOCATION(),
                                        "RemoveInvalidWasPresent::lowerLevel");
    lowerLevel.lock();
    higherLevel.unlock();
}

TEST_F(LatchAnalyzerTest, AddValidWasAbsent) {
    Mutex higherLevel = MONGO_MAKE_LATCH(
        Level(2), (SourceLocationHolder)MONGO_SOURCE_LOCATION(), "AddValidWasAbsent::higherLevel");
    higherLevel.lock();
    Mutex lowerLevel = MONGO_MAKE_LATCH(
        Level(1), (SourceLocationHolder)MONGO_SOURCE_LOCATION(), "AddValidWasAbsent::lowerLevel");
    lowerLevel.lock();
}

TEST_F(LatchAnalyzerTest, RemoveValidWasPresent) {

    Mutex higherLevel = MONGO_MAKE_LATCH(Level(2),
                                         (SourceLocationHolder)MONGO_SOURCE_LOCATION(),
                                         "RemoveValidWasPresent::higherLevel");
    higherLevel.lock();
    Mutex lowerLevel = MONGO_MAKE_LATCH(Level(1),
                                        (SourceLocationHolder)MONGO_SOURCE_LOCATION(),
                                        "RemoveValidWasPresent::lowerLevel");
    lowerLevel.lock();

    lowerLevel.unlock();
    higherLevel.unlock();
}

}  // namespace
}  // namespace mongo
