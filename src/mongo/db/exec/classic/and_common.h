// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/util/modules.h"

namespace mongo {

class AndCommon {
public:
    /**
     * If 'src' has any data that the member in 'workingSet' keyed by 'destId' doesn't, add that
     * data to 'destId's WSM.
     */
    static void mergeFrom(WorkingSet* workingSet,
                          WorkingSetID destId,
                          const WorkingSetMember& src) {
        WorkingSetMember* dest = workingSet->get(destId);

        // Both 'src' and 'dest' must have a RecordId (and they must be the same RecordId), as
        // we should have just matched them according to this RecordId while doing an
        // intersection.
        MONGO_verify(dest->hasRecordId());
        MONGO_verify(src.hasRecordId());
        MONGO_verify(dest->recordId == src.recordId);

        dest->metadata().mergeWith(src.metadata());

        if (dest->hasObj()) {
            // The merged WSM that we're creating already has the full document, so there's
            // nothing left to do.
            return;
        }

        if (src.hasObj()) {
            tassert(11051668,
                    "Expecting src state to be RID_AND_OBJ",
                    src.getState() == WorkingSetMember::RID_AND_OBJ);

            // 'src' has the full document but 'dest' doesn't so we need to copy it over.
            dest->doc = src.doc;
            dest->makeObjOwnedIfNeeded();

            // We have an object so we don't need key data.
            dest->keyData.clear();

            workingSet->transitionToRecordIdAndObj(destId);

            // Now 'dest' has the full object. No more work to do.
            return;
        }

        // If we're here, then both WSMs getting merged contain index keys. We need
        // to merge the key data.
        //
        // This is N^2 but N is probably pretty small.  Easy enough to revisit.
        for (size_t i = 0; i < src.keyData.size(); ++i) {
            bool found = false;
            for (size_t j = 0; j < dest->keyData.size(); ++j) {
                if (SimpleBSONObjComparator::kInstance.evaluate(dest->keyData[j].indexKeyPattern ==
                                                                src.keyData[i].indexKeyPattern)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                dest->keyData.push_back(src.keyData[i]);
            }
        }
    }
};

}  // namespace mongo
