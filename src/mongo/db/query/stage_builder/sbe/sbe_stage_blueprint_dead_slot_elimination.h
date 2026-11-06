// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_rewrite.h"

#include <memory>

namespace mongo::stage_builder {
/**
 * Returns a rule that removes project entries whose outputs nothing reads, and replaces a project
 * left with no entries by its child. The rule needs the slot summary in the rewrite context and
 * keeps it up to date.
 */
std::unique_ptr<SbBlueprintRewriteRule> makeDeadSlotEliminationRule();
}  // namespace mongo::stage_builder
