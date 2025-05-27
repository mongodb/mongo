/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/primary_only_service_helpers/cancel_state.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace primary_only_service_helpers {
namespace {

class CancelStateTest : public unittest::Test {
protected:
    CancelStateTest() : _stepdownToken{_stepdownSource.token()}, _cancelState{_stepdownToken} {}

    CancelState& getCancelState() {
        return _cancelState;
    }

    void stepdown() {
        _stepdownSource.cancel();
    }

private:
    CancellationSource _stepdownSource;
    CancellationToken _stepdownToken;
    CancelState _cancelState;
};

TEST_F(CancelStateTest, NotSteppingDown) {
    ASSERT_FALSE(getCancelState().isSteppingDown());
}

TEST_F(CancelStateTest, IsSteppingDownIndicatesStepdown) {
    stepdown();
    ASSERT_TRUE(getCancelState().isSteppingDown());
}

TEST_F(CancelStateTest, IsSteppingDownUnaffectedByAbort) {
    getCancelState().abort();
    ASSERT_FALSE(getCancelState().isSteppingDown());
}

TEST_F(CancelStateTest, NotAbortedOrSteppingDown) {
    ASSERT_FALSE(getCancelState().isAbortedOrSteppingDown());
}

TEST_F(CancelStateTest, IsAbortedOrSteppingDownIndicatesAbort) {
    getCancelState().abort();
    ASSERT_TRUE(getCancelState().isAbortedOrSteppingDown());
}

TEST_F(CancelStateTest, IsAbortedOrSteppingDownIndicatesStepdown) {
    stepdown();
    ASSERT_TRUE(getCancelState().isAbortedOrSteppingDown());
}

}  // namespace
}  // namespace primary_only_service_helpers
}  // namespace mongo
