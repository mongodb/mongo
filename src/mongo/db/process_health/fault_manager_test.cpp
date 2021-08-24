/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/process_health/fault_manager.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

namespace process_health {

namespace {

TEST(FaultManagerTest, Registration) {
    auto serviceCtx = ServiceContext::make();
    ASSERT_TRUE(FaultManager::get(serviceCtx.get()));
}

class FaultManagerTestImpl : public FaultManager {
public:
    FaultManagerTestImpl(ServiceContext* svcCtx) : FaultManager(svcCtx) {}

    Status transitionStateTest(FaultState newState) {
        return transitionToState(newState);
    }

    FaultState getFaultStateTest() {
        return getFaultState();
    }
};

// State machine tests.
TEST(FaultManagerForTest, StateTransitionsFromOk) {
    auto serviceCtx = ServiceContext::make();
    std::vector<std::pair<FaultState, bool>> transitionValidPairs{
        {FaultState::kOk, false},
        {FaultState::kStartupCheck, false},
        {FaultState::kTransientFault, true},
        {FaultState::kActiveFault, false}};

    for (auto& pair : transitionValidPairs) {
        FaultManagerTestImpl faultManager(serviceCtx.get());
        ASSERT_OK(faultManager.transitionStateTest(FaultState::kOk));

        if (pair.second) {
            ASSERT_OK(faultManager.transitionStateTest(pair.first));
        } else {
            ASSERT_NOT_OK(faultManager.transitionStateTest(pair.first));
        }
    }
}

TEST(FaultManagerForTest, StateTransitionsFromStartupCheck) {
    auto serviceCtx = ServiceContext::make();
    std::vector<std::pair<FaultState, bool>> transitionValidPairs{
        {FaultState::kOk, true},
        {FaultState::kStartupCheck, false},
        {FaultState::kTransientFault, true},
        {FaultState::kActiveFault, false}};

    for (auto& pair : transitionValidPairs) {
        FaultManagerTestImpl faultManager(serviceCtx.get());

        if (pair.second) {
            ASSERT_OK(faultManager.transitionStateTest(pair.first));
        } else {
            ASSERT_NOT_OK(faultManager.transitionStateTest(pair.first));
        }
    }
}

TEST(FaultManagerForTest, StateTransitionsFromTransientFault) {
    auto serviceCtx = ServiceContext::make();
    std::vector<std::pair<FaultState, bool>> transitionValidPairs{
        {FaultState::kOk, true},
        {FaultState::kStartupCheck, false},
        {FaultState::kTransientFault, false},
        {FaultState::kActiveFault, true}};

    for (auto& pair : transitionValidPairs) {
        FaultManagerTestImpl faultManager(serviceCtx.get());
        ASSERT_OK(faultManager.transitionStateTest(FaultState::kTransientFault));

        if (pair.second) {
            ASSERT_OK(faultManager.transitionStateTest(pair.first));
        } else {
            ASSERT_NOT_OK(faultManager.transitionStateTest(pair.first));
        }
    }
}

TEST(FaultManagerForTest, StateTransitionsFromActiveFault) {
    auto serviceCtx = ServiceContext::make();
    std::vector<std::pair<FaultState, bool>> transitionValidPairs{
        {FaultState::kOk, false},
        {FaultState::kStartupCheck, false},
        {FaultState::kTransientFault, false},
        {FaultState::kActiveFault, false}};

    for (auto& pair : transitionValidPairs) {
        FaultManagerTestImpl faultManager(serviceCtx.get());
        ASSERT_OK(faultManager.transitionStateTest(FaultState::kTransientFault));
        ASSERT_OK(faultManager.transitionStateTest(FaultState::kActiveFault));

        if (pair.second) {
            ASSERT_OK(faultManager.transitionStateTest(pair.first));
        } else {
            ASSERT_NOT_OK(faultManager.transitionStateTest(pair.first));
        }
    }
}
}  // namespace
}  // namespace process_health
}  // namespace mongo
