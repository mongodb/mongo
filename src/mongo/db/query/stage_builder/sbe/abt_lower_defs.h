// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/util/modules.h"


namespace mongo::stage_builder::abt_lower {

using SlotVarMap =
    stdx::unordered_map<abt::ProjectionName, sbe::value::SlotId, abt::ProjectionName::Hasher>;

}  // namespace mongo::stage_builder::abt_lower
