// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"

namespace mongo {

// Rejects the sentinel value 13 to exercise callback validators in QueryKnob unit tests.
Status validateTestIntKnobCallback(const int& value, const boost::optional<TenantId>&);

}  // namespace mongo
