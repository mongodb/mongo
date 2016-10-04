/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

class CanonicalQuery;
class Database;
class DeleteRequest;
class OperationContext;

/**
 * This class takes a pointer to a DeleteRequest, and converts that request into a parsed form
 * via the parseRequest() method. A ParsedDelete can then be used to retrieve a PlanExecutor
 * capable of executing the delete.
 *
 * It is invalid to request that the DeleteStage return the deleted document during a
 * multi-remove. It is also invalid to request that a ProjectionStage be applied to the
 * DeleteStage if the DeleteStage would not return the deleted document.
 *
 * A delete request is parsed to a CanonicalQuery, so this class is a thin, delete-specific
 * wrapper around canonicalization.
 *
 * No locks need to be held during parsing.
 */
class ParsedDelete {
    MONGO_DISALLOW_COPYING(ParsedDelete);

public:
    /**
     * Constructs a parsed delete.
     *
     * The object pointed to by "request" must stay in scope for the life of the constructed
     * ParsedDelete.
     */
    ParsedDelete(OperationContext* txn, const DeleteRequest* request);

    /**
     * Parses the delete request to a canonical query. On success, the parsed delete can be
     * used to create a PlanExecutor capable of executing this delete.
     */
    Status parseRequest();

    /**
     * As an optimization, we do not create a canonical query if the predicate is a simple
     * _id equality. This method can be used to force full parsing to a canonical query,
     * as a fallback if the idhack path is not available (e.g. no _id index).
     */
    Status parseQueryToCQ();

    /**
     * Get the raw request.
     */
    const DeleteRequest* getRequest() const;

    /**
     * Get the YieldPolicy, adjusted for $isolated and GodMode.
     */
    PlanExecutor::YieldPolicy yieldPolicy() const;

    /**
     * Is this update supposed to be isolated?
     */
    bool isIsolated() const;

    /**
     * As an optimization, we don't create a canonical query for updates with simple _id
     * queries. Use this method to determine whether or not we actually parsed the query.
     */
    bool hasParsedQuery() const;

    /**
     * Releases ownership of the canonical query to the caller.
     */
    std::unique_ptr<CanonicalQuery> releaseParsedQuery();

private:
    // Transactional context.  Not owned by us.
    OperationContext* _txn;

    // Unowned pointer to the request object that this executor will process.
    const DeleteRequest* const _request;

    // Parsed query object, or NULL if the query proves to be an id hack query.
    std::unique_ptr<CanonicalQuery> _canonicalQuery;
};

}  // namespace mongo
