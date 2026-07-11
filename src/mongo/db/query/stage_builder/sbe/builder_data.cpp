// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/builder_data.h"

namespace mongo::stage_builder {

std::string PlanStageData::debugString(boost::optional<size_t> lengthCap /*= boost::none*/) const {
    StringBuilder builder;
    StringBuilder tmp;

    if (auto slot = staticData->resultSlot) {
        tmp << "$$RESULT=s" << *slot << " ";
        if (lengthCap.has_value() && static_cast<size_t>(builder.len() + tmp.len()) > lengthCap) {
            builder << "...";
            return builder.str();
        }
        builder << tmp.str();
        tmp.reset();
    }
    if (auto slot = staticData->recordIdSlot) {
        builder << "$$RID=s" << *slot << " ";
        if (static_cast<size_t>(builder.len() + tmp.len()) > lengthCap) {
            builder << "...";
            return builder.str();
        }
        builder << tmp.str();
        tmp.reset();
    }

    if (lengthCap.has_value() && static_cast<size_t>(builder.len()) >= lengthCap.get()) {
        builder << "...";
        return builder.str();
    }
    boost::optional<size_t> curLengthCap = lengthCap.has_value()
        ? boost::make_optional(lengthCap.get() - static_cast<size_t>(builder.len()))
        : boost::none;
    env->debugString(&builder, curLengthCap);

    return builder.str();
}


}  // namespace mongo::stage_builder
