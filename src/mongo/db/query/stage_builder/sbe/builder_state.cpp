/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/stage_builder/sbe/builder_state.h"

#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/in_list.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"

namespace mongo::stage_builder {
sbe::value::SlotId StageBuilderState::getGlobalVariableSlot(Variables::Id variableId) {
    if (auto it = data->variableIdToSlotMap.find(variableId);
        it != data->variableIdToSlotMap.end()) {
        return it->second;
    }

    auto slotId =
        env->registerSlot(sbe::value::TypeTags::Nothing, 0, false /* owned */, slotIdGenerator);
    data->variableIdToSlotMap.emplace(variableId, slotId);
    return slotId;
}

const CollatorInterface* StageBuilderState::makeOwnedCollator(const CollatorInterface* coll) {
    if (!coll) {
        return nullptr;
    }

    auto queryColl = data->queryCollator.get();
    if (coll == queryColl || CollatorInterface::collatorsMatch(coll, queryColl)) {
        return queryColl;
    }

    if (auto it = collatorsMap->find(coll); it != collatorsMap->end()) {
        return it->second;
    }

    data->collators.emplace_back(coll->clone());
    auto clonedColl = data->collators.back().get();

    (*collatorsMap)[coll] = clonedColl;
    (*collatorsMap)[clonedColl] = clonedColl;
    return clonedColl;
}

sbe::InList* StageBuilderState::makeOwnedInList(const InMatchExpression* ime) {
    if (inListsMap) {
        if (auto it = inListsMap->find(ime); it != inListsMap->end()) {
            return it->second;
        }
    }

    // Get the InMatchExpression's InListData.
    std::shared_ptr<const InListData> inListData = ime->getInListDataPtr();

    // If the InListData's BSON is not owned, clone the InListData object and make its BSON owned.
    if (!inListData->isBSONOwned()) {
        std::shared_ptr<InListData> clonedData = inListData->clone();
        clonedData->makeBSONOwned();
        clonedData->setShared();

        inListData = std::move(clonedData);
    }

    const CollatorInterface* coll = inListData->getCollator();

    // If 'coll' is not null, make it owned.
    if (coll != nullptr) {
        coll = makeOwnedCollator(coll);
    }

    // Create an InList from 'inListData' and 'coll'.
    auto inListPtr = std::make_unique<sbe::InList>(std::move(inListData), coll);
    sbe::InList* inList = inListPtr.get();

    // Store 'inListPtr' into the PlanStageStaticData.
    data->inLists.emplace_back(std::move(inListPtr));

    // Add 'inList' to 'inListsMap' so that we can retrieve it if makeOwnedInList() is called
    // again on the same InMatchExpression.
    if (inListsMap) {
        inListsMap->emplace(ime, inList);
    }

    return inList;
}

sbe::value::SlotId StageBuilderState::getSortSpecSlot(const AccumulationStatement* acc) {
    tassert(8679706, "Expected non-null AccumulationStatement", acc != nullptr);
    const void* key = static_cast<const void*>(acc);

    auto it = sortSpecMap->find(key);
    if (it != sortSpecMap->end()) {
        auto slot = it->second;
        return slot;
    }

    // If we don't have a SortSpec for this AccumulationStatement yet, create one and
    // add it the map and return it.
    auto sortSpec = makeSortSpecFromSortPattern(getSortPattern(*acc));

    auto tag = sbe::value::TypeTags::sortSpec;
    auto val = sbe::value::bitcastFrom<sbe::SortSpec*>(sortSpec.release());
    auto slot = env->registerSlot(tag, val, true, slotIdGenerator);

    (*sortSpecMap)[key] = slot;
    return slot;
}

sbe::value::SlotId StageBuilderState::getSortSpecSlot(const WindowFunctionStatement* wf) {
    tassert(8679707, "Expected non-null WindowFunctionStatement", wf != nullptr);
    const void* key = static_cast<const void*>(wf);

    auto it = sortSpecMap->find(key);
    if (it != sortSpecMap->end()) {
        auto slot = it->second;
        return slot;
    }

    // If we don't have a SortSpec for this WindowFunctionStatement yet, create one
    // and add it the map and return it.
    auto sortSpec = makeSortSpecFromSortPattern(getSortPattern(*wf));

    auto tag = sbe::value::TypeTags::sortSpec;
    auto val = sbe::value::bitcastFrom<sbe::SortSpec*>(sortSpec.release());
    auto slot = env->registerSlot(tag, val, true, slotIdGenerator);

    (*sortSpecMap)[key] = slot;
    return slot;
}

sbe::value::SlotId StageBuilderState::registerInputParamSlot(
    MatchExpression::InputParamId paramId) {
    auto it = data->inputParamToSlotMap.find(paramId);
    if (it != data->inputParamToSlotMap.end()) {
        // This input parameter id has already been tied to a particular runtime environment slot.
        // Just return that slot to the caller. This can happen if a query planning optimization or
        // rewrite chose to clone one of the input expressions from the user's query.
        return it->second;
    }

    auto slotId =
        env->registerSlot(sbe::value::TypeTags::Nothing, 0, false /* owned */, slotIdGenerator);
    data->inputParamToSlotMap.emplace(paramId, slotId);
    return slotId;
}

sbe::value::SlotId StageBuilderState::getNothingSlot() {
    auto slotId = env->getSlotIfExists(kNothingEnvSlotName);

    if (!slotId) {
        return env->registerSlot(
            kNothingEnvSlotName, sbe::value::TypeTags::Nothing, 0, false, slotIdGenerator);
    }

    return *slotId;
}

bool StageBuilderState::isNothingSlot(sbe::value::SlotId slot) {
    return slot == env->getSlotIfExists(kNothingEnvSlotName);
}

sbe::value::SlotId StageBuilderState::getEmptyObjSlot() {
    auto slotId = env->getSlotIfExists("emptyObj"_sd);

    if (!slotId) {
        auto tag = sbe::value::TypeTags::bsonObject;
        auto val = sbe::value::bitcastFrom<const char*>(BSONObj::kEmptyObject.objdata());
        return env->registerSlot("emptyObj"_sd, tag, val, false, slotIdGenerator);
    }

    return *slotId;
}

boost::optional<sbe::value::SlotId> StageBuilderState::getTimeZoneDBSlot() {
    auto slotId = env->getSlotIfExists("timeZoneDB"_sd);

    if (!slotId) {
        return env->registerSlot(
            "timeZoneDB"_sd,
            sbe::value::TypeTags::timeZoneDB,
            sbe::value::bitcastFrom<const TimeZoneDatabase*>(getTimeZoneDatabase(opCtx)),
            false,
            slotIdGenerator);
    }

    return slotId;
}

boost::optional<sbe::value::SlotId> StageBuilderState::getCollatorSlot() {
    auto slotId = env->getSlotIfExists("collator"_sd);

    if (!slotId && data != nullptr) {
        if (auto coll = data->queryCollator.get()) {
            return env->registerSlot("collator"_sd,
                                     sbe::value::TypeTags::collator,
                                     sbe::value::bitcastFrom<const CollatorInterface*>(coll),
                                     false,
                                     slotIdGenerator);
        }
    }

    return slotId;
}

boost::optional<sbe::value::SlotId> StageBuilderState::getOplogTsSlot() {
    auto slotId = env->getSlotIfExists("oplogTs"_sd);

    if (!slotId) {
        return env->registerSlot(
            "oplogTs"_sd, sbe::value::TypeTags::Nothing, 0, false, slotIdGenerator);
    }

    return slotId;
}

boost::optional<sbe::value::SlotId> StageBuilderState::getBuiltinVarSlot(Variables::Id id) {
    if (id == Variables::kRootId || id == Variables::kRemoveId) {
        return boost::none;
    }

    auto it = Variables::kIdToBuiltinVarName.find(id);
    tassert(7690415, "Expected 'id' to be in map", it != Variables::kIdToBuiltinVarName.end());

    auto& name = it->second;
    auto slotId = env->getSlotIfExists(name);
    if (!slotId) {
        if (variables.hasValue(id)) {
            auto [tag, val] = sbe::value::makeValue(variables.getValue(id));
            return env->registerSlot(name, tag, val, true, slotIdGenerator);
        } else if (id == Variables::kSearchMetaId) {
            // Normally, $search is responsible for setting a value for SEARCH_META, in which case
            // we will bind the value to a slot above. However, in the event of a query that does
            // not use $search, but references SEARCH_META, we need to bind a value of 'missing' to
            // a slot so that the plan can run correctly.
            return env->registerSlot(
                name, sbe::value::TypeTags::Nothing, 0, false, slotIdGenerator);
        }
    }

    return slotId;
}
}  // namespace mongo::stage_builder
