// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_settings/query_knob_overrides.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/util/modules.h"

namespace mongo::query_settings {

size_t hash_value(const QuerySettingsKnobOverrides& overrides);

/**
 * Computes hash of 'querySettings'.
 */
size_t hash(const QuerySettings& querySettings);
}  // namespace mongo::query_settings
