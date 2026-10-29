// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/classic/return_key.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
PlanStage::StageState ReturnKeyStage::doWork(WorkingSetID* out) {
    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState status = child()->work(&id);

    if (PlanStage::ADVANCED == status) {
        WorkingSetMember* member = _ws.get(id);
        _extractIndexKey(member);
        *out = id;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    return status;
}

std::unique_ptr<PlanStageStats> ReturnKeyStage::getStats() {
    _commonStats.isEOF = isEOF();

    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::make_unique<ReturnKeyStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

void ReturnKeyStage::_extractIndexKey(WorkingSetMember* member) {
    if (!_sortKeyMetaFields.empty()) {
        tassert(11051632,
                "Expecting working set member metadata to have sort key",
                member->metadata().hasSortKey());
    }

    auto indexKey = member->metadata().hasIndexKey() ? member->metadata().getIndexKey() : BSONObj();

    MutableDocument md;

    for (auto&& elem : indexKey) {
        md.addField(elem.fieldNameStringData(), Value(elem));
    }

    for (const auto& fieldPath : _sortKeyMetaFields) {
        if (!member->metadata().hasSortKey()) {
            md.setNestedField(fieldPath, Value{});
            continue;
        }

        md.setNestedField(
            fieldPath,
            Value(DocumentMetadataFields::serializeSortKey(member->metadata().isSingleElementKey(),
                                                           member->metadata().getSortKey())));
    }

    member->keyData.clear();
    member->recordId = {};
    member->doc = {{}, md.freeze()};
    member->transitionToOwnedObj();
}
}  // namespace mongo
