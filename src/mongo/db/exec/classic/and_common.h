/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/classic/working_set.h"

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
            invariant(src.getState() == WorkingSetMember::RID_AND_OBJ);

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
