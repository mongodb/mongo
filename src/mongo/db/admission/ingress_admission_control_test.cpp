// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

using IngressAdmissionControllerTestDeathTest = IngressAdmissionControllerTest;
DEATH_TEST_F(IngressAdmissionControllerTestDeathTest,
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
