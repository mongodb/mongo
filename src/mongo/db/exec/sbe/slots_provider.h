// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::sbe {

class SlotsProvider {
public:
    virtual sbe::value::SlotId getSlot(std::string_view name) const = 0;
    virtual boost::optional<sbe::value::SlotId> getSlotIfExists(std::string_view name) const = 0;
    virtual sbe::value::SlotId registerSlot(sbe::value::TypeTags tag,
                                            sbe::value::Value val,
                                            bool owned,
                                            sbe::value::SlotIdGenerator* slotIdGenerator) = 0;
};

}  // namespace mongo::sbe
