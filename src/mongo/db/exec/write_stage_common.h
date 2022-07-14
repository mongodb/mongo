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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class CanonicalQuery;
class Collection;
class CollectionPtr;
class OperationContext;
class BSONObj;
class Document;

namespace write_stage_common {

class PreWriteFilter {
public:
    /**
     * This class represents the different kind of actions we can take when handling a write
     * operation:
     *   - kWrite: perform the current write operation.
     *   - kWriteAsFromMigrate: perform the current write operation but marking it with the
     *     fromMigrate flag.
     *   - kSkip: skip the current write operation.
     */
    enum class Action { kWrite, kWriteAsFromMigrate, kSkip };

    PreWriteFilter(OperationContext* opCtx, NamespaceString nss);

    void saveState() {}

    void restoreState();

    /**
     * Returns which PreWriteFilterAction we should take for the current write operation over doc.
     */
    Action computeAction(const Document& doc);

private:
    /**
     * Returns true if the operation is not versioned or if the doc is owned by the shard.
     *
     * May thow a ShardKeyNotFound if the document has an invalid shard key.
     */
    bool _documentBelongsToMe(const BSONObj& doc);

    OperationContext* _opCtx;
    NamespaceString _nss;
    const bool _skipFiltering;
    std::unique_ptr<ShardFilterer> _shardFilterer;
};

/**
 * Returns true if the document referred to by 'id' still exists and matches the query predicate
 * given by 'cq'. Returns true if the document still exists and 'cq' is null. Returns false
 * otherwise.
 *
 * May throw a WriteConflictException if there was a conflict while searching to see if the document
 * still exists.
 */
bool ensureStillMatches(const CollectionPtr& collection,
                        OperationContext* opCtx,
                        WorkingSet* ws,
                        WorkingSetID id,
                        const CanonicalQuery* cq);
}  // namespace write_stage_common
}  // namespace mongo
