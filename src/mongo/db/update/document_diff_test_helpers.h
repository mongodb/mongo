// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/platform/random.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo::doc_diff {

BSONObj createObjWithLargePrefix(const std::string& suffix);

BSONObj generateDoc(PseudoRandom* rng, MutableDocument* doc, int depthLevel);

BSONObj applyDiffTestHelper(BSONObj preImage,
                            BSONObj diff,
                            bool mustCheckExistenceForInsertOperations = true);
}  // namespace mongo::doc_diff
