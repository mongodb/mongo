// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

class ServiceContext;

namespace admission {
namespace [[MONGO_MOD_PUBLIC]] execution_control {

/**
 * Globally initialize the ticketing system from execution control.
 */
void initializeTicketingSystem(ServiceContext* svCctx);
}  // namespace execution_control
}  // namespace admission

}  // namespace mongo
