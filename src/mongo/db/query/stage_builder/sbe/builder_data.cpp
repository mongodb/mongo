// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/builder_data.h"

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/util/assert_util.h"

namespace mongo::stage_builder {
namespace {
std::string_view toString(sbe::ResultFieldAction action) {
    switch (action) {
        case sbe::ResultFieldAction::kKeep:
            return "keep";
        case sbe::ResultFieldAction::kDrop:
            return "drop";
        case sbe::ResultFieldAction::kSet:
            return "set";
        case sbe::ResultFieldAction::kAdd:
            return "add";
    }
    MONGO_UNREACHABLE_TASSERT(13843601);
}
}  // namespace

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
    if (const auto& changes = staticData->resultChanges) {
        tmp << "$$RESULT_CHANGES=" << (changes->closed ? "closed" : "open") << "{";
        bool first = true;
        for (const auto& field : changes->fields) {
            tmp << (first ? "" : ",") << field.name << "=" << toString(field.action);
            if (field.slot) {
                tmp << "(s" << *field.slot << ")";
            }
            first = false;
        }
        tmp << "} ";
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

sbe::ResultMaterializer bindResultMaterializer(sbe::PlanStage* root,
                                               sbe::CompileCtx& ctx,
                                               const PlanStageStaticData& data) {
    tassert(13843602, "Expected the plan to have a result slot", data.resultSlot);
    auto* base = root->getAccessor(ctx, *data.resultSlot);
    tassert(13843603, "Query does not have result slot.", base);

    if (!data.resultChanges) {
        return sbe::ResultMaterializer(base, false /* closed */, {});
    }

    std::vector<sbe::ResultField> fields;
    fields.reserve(data.resultChanges->fields.size());
    for (const auto& change : data.resultChanges->fields) {
        sbe::value::SlotAccessor* accessor = nullptr;
        if (change.slot) {
            accessor = root->getAccessor(ctx, *change.slot);
            tassert(13843604,
                    str::stream() << "Query does not have a slot for result field '" << change.name
                                  << "'",
                    accessor);
        }
        fields.emplace_back(change.name, change.action, accessor);
    }
    return sbe::ResultMaterializer(base, data.resultChanges->closed, std::move(fields));
}

}  // namespace mongo::stage_builder
