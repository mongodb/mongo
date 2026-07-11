// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"

namespace mongo::sbe::value {
class SlotAccessorHelper {
public:
    /**
     * Asserts that a value has not been invalidated.
     */
    MONGO_COMPILER_ALWAYS_INLINE static void dassertValidSlotValue(TypeTags tag, Value val) {
#if defined(MONGO_CONFIG_DEBUG_BUILD)
        tassert(7200401,
                str::stream() << "Unexpected poison value. Likely attempted to access a slot that "
                                 "was disabled and invalidated in save/restore cycle.",
                !value::isPoisonValue(tag, val));
#endif
    }
};
}  // namespace mongo::sbe::value
