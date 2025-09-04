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
