// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

namespace mongo {

class CollectionPtr;
class OperationContext;
class SeekableRecordCursor;

class WorkingSetCommon {
public:
    /**
     * Transitions the WorkingSetMember with WorkingSetID 'id' from the RID_AND_IDX state to the
     * RID_AND_OBJ state by fetching a document. Does the fetch using 'cursor'.
     *
     * If false is returned, the document should not be considered for the result set. It is the
     * caller's responsibility to free 'id' in this case.
     *
     * WriteConflict exceptions may be thrown. When they are, 'member' will be unmodified.
     */
    static bool fetch(OperationContext* opCtx,
                      WorkingSet* workingSet,
                      WorkingSetID id,
                      SeekableRecordCursor* cursor,
                      const CollectionPtr& collection,
                      const NamespaceString& ns);
};

}  // namespace mongo
