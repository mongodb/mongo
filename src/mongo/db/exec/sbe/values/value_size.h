// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe::value {

int getApproximateSize(TypeTags tag, Value val);

}  // namespace mongo::sbe::value
