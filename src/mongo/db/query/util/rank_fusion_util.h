/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

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
