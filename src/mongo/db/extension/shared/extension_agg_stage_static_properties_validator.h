// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo::extension {
class MongoExtensionDocsNeededBoundsInfo;

/**
 * Validates that 'value' is provided when 'effect' is "limit" or "skip", and is absent otherwise.
 */
void validateDocsNeededBoundsInfo(const MongoExtensionDocsNeededBoundsInfo* info);

}  // namespace mongo::extension
