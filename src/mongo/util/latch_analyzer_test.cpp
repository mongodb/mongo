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
#include "mongo/util/latch_analyzer.h"

namespace mongo {
namespace {

using Level = HierarchicalAcquisitionLevel;

class LatchAnalyzerTest : public ServiceContextTest {};

DEATH_TEST_REGEX_F(LatchAnalyzerTest, AddInvalidWasAbsent, "Fatal assertion.*5106800") {

    Mutex lowerLevel = MONGO_MAKE_LATCH(Level(1), "AddInvalidWasAbsent::lowerLevel");
    Mutex higherLevel = MONGO_MAKE_LATCH(Level(2), "AddInvalidWasAbsent::higherLevel");

    lowerLevel.lock();
    higherLevel.lock();
}

DEATH_TEST_REGEX_F(LatchAnalyzerTest, AddInvalidWasPresent, "Fatal assertion.*5106801") {
    Mutex m1 = MONGO_MAKE_LATCH(Level(1), "AddInvalidWasPresent::m1");
    Mutex m2 = MONGO_MAKE_LATCH(Level(1), "AddInvalidWasPresent::m2");

    m1.lock();
    m2.lock();
}

DEATH_TEST_REGEX_F(LatchAnalyzerTest, RemoveInvalidWasAbsent, "Fatal assertion.*5106802") {
    Mutex m = MONGO_MAKE_LATCH(Level(1), "RemoveInvalidWasAbsent::m");

    m.unlock();
}

DEATH_TEST_REGEX_F(LatchAnalyzerTest, RemoveInvalidWasPresent, "Fatal assertion.*5106803") {
    Mutex higherLevel = MONGO_MAKE_LATCH(Level(2), "RemoveInvalidWasPresent::higherLevel");
    Mutex lowerLevel = MONGO_MAKE_LATCH(Level(1), "RemoveInvalidWasPresent::lowerLevel");

    higherLevel.lock();
    lowerLevel.lock();
    higherLevel.unlock();
}

TEST_F(LatchAnalyzerTest, AddValidWasAbsent) {
    Mutex higherLevel = MONGO_MAKE_LATCH(Level(2), "AddValidWasAbsent::higherLevel");
    Mutex lowerLevel = MONGO_MAKE_LATCH(Level(1), "AddValidWasAbsent::lowerLevel");

    higherLevel.lock();
    lowerLevel.lock();

    {
        LatchAnalyzerDisabledBlock block;
        higherLevel.unlock();
        lowerLevel.unlock();
    }
}

TEST_F(LatchAnalyzerTest, RemoveValidWasPresent) {

    Mutex higherLevel = MONGO_MAKE_LATCH(Level(2), "RemoveValidWasPresent::higherLevel");
    Mutex lowerLevel = MONGO_MAKE_LATCH(Level(1), "RemoveValidWasPresent::lowerLevel");

    {
        LatchAnalyzerDisabledBlock block;
        higherLevel.lock();
        lowerLevel.lock();
    }

    lowerLevel.unlock();
    higherLevel.unlock();
}

}  // namespace
}  // namespace mongo
