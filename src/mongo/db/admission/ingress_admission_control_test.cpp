/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/admission/ingress_admission_controller.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cmath>
#include <functional>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

class IngressAdmissionControllerTest : public ServiceContextTest {};

DEATH_TEST_F(IngressAdmissionControllerTest,
             SameOpCannotAcquireMultipleTickets,
             "Tripwire assertion") {
    auto opCtx = makeOperationContext();
    auto& admissionController = IngressAdmissionController::get(opCtx.get());
    auto ticket = admissionController.admitOperation(opCtx.get());

    // The way ingress admission works, one ticket should cover _all_ the work for the operation.
    // Therefore, if the operation has already been admitted by IngressAdmissionController, all
    // subsequent admissions of the same operation should be exempt and not take tickets out of the
    // pool.
    ASSERT_THROWS_CODE(
        admissionController.admitOperation(opCtx.get()), AssertionException, 9143000);
}

}  // namespace
}  // namespace mongo
