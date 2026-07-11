// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <span>

namespace mongo {

class Collection;
class CollectionPtr;
class OperationContext;

namespace repl {

/**
 * Used on a local Collection to create and bulk build indexes.
 *
 * Note that no methods on this class are thread-safe.
 */
class CollectionBulkLoader {
public:
    // A function that returns the recordId and original document from
    // a projected find query.
    using ParseRecordIdAndDocFunc = function_ref<std::pair<RecordId, BSONObj>(const BSONObj&)>;
    virtual ~CollectionBulkLoader() = default;

    /**
     * Inserts the documents into the collection record store, and indexes them with the
     * MultiIndexBlock on the side.
     *
     * If the stream of BSONObj provided requires transformation to pull out the original
     * recordId and original document, 'fn' can be provided to perform that transformation.
     */
    virtual Status insertDocuments(std::span<BSONObj> objs,
                                   ParseRecordIdAndDocFunc fn = defaultParseRecordIdAndDocFunc) = 0;
    /**
     * Called when inserts are done and indexes can be committed.
     */
    virtual Status commit() = 0;

private:
    static std::pair<RecordId, BSONObj> defaultParseRecordIdAndDocFunc(const BSONObj& obj) {
        return {RecordId(0), obj};
    }
};

}  // namespace repl
}  // namespace mongo
