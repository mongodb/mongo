// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

/**
 * Returns true if the full rank fusion feature is enabled.
 *
 * This check ORs the value of 'featureFlagRankFusionFull' and the 'bypassRankFusionFCVGate' query
 * knob. This complexity is to support our unique backporting strategy for this feature. Because
 * this feature will be enabled by default in _some_ previous versions, we want to bypass FCV-gating
 * for this feature flag for some portion of the fleet, while maintaining FCV-gating as the default
 * behavior.
 *
 * Specifically, 8.0 clusters that have access to $rankFusion features via the backported feature
 * will lose access to them during an FCV-gated upgrade. We want to be able to bypass FCV-gating for
 * those users without disabling FCV-gating for users coming from 8.0 versions without $rankFusion.
 *
 * TODO SERVER-85426 Remove this function/file.
 */
bool isRankFusionFullEnabled();
}  // namespace mongo
