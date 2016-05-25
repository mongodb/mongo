/**
 *    Copyright (C) 2015 10gen Inc.
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

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class QueryPlannerTest : public mongo::unittest::Test {
protected:
    void setUp();

    OperationContext* txn();

    //
    // Build up test.
    //

    void addIndex(BSONObj keyPattern, bool multikey = false);

    void addIndex(BSONObj keyPattern, bool multikey, bool sparse);

    void addIndex(BSONObj keyPattern, bool multikey, bool sparse, bool unique);

    void addIndex(BSONObj keyPattern, BSONObj infoObj);

    void addIndex(BSONObj keyPattern, MatchExpression* filterExpr);

    void addIndex(BSONObj keyPattern, const MultikeyPaths& multikeyPaths);

    void addIndex(BSONObj keyPattern, const CollatorInterface* collator);

    void addIndex(BSONObj keyPattern,
                  MatchExpression* filterExpr,
                  const CollatorInterface* collator);

    //
    // Execute planner.
    //

    void runQuery(BSONObj query);

    void runQuerySortProj(const BSONObj& query, const BSONObj& sort, const BSONObj& proj);

    void runQuerySkipNToReturn(const BSONObj& query, long long skip, long long ntoreturn);

    void runQueryHint(const BSONObj& query, const BSONObj& hint);

    void runQuerySortProjSkipNToReturn(const BSONObj& query,
                                       const BSONObj& sort,
                                       const BSONObj& proj,
                                       long long skip,
                                       long long ntoreturn);

    void runQuerySortHint(const BSONObj& query, const BSONObj& sort, const BSONObj& hint);

    void runQueryHintMinMax(const BSONObj& query,
                            const BSONObj& hint,
                            const BSONObj& minObj,
                            const BSONObj& maxObj);

    void runQuerySortProjSkipNToReturnHint(const BSONObj& query,
                                           const BSONObj& sort,
                                           const BSONObj& proj,
                                           long long skip,
                                           long long ntoreturn,
                                           const BSONObj& hint);

    void runQuerySnapshot(const BSONObj& query);

    void runQueryFull(const BSONObj& query,
                      const BSONObj& sort,
                      const BSONObj& proj,
                      long long skip,
                      long long ntoreturn,
                      const BSONObj& hint,
                      const BSONObj& minObj,
                      const BSONObj& maxObj,
                      bool snapshot);

    //
    // Same as runQuery* functions except we expect a failed status from the planning stage.
    //

    void runInvalidQuery(const BSONObj& query);

    void runInvalidQuerySortProj(const BSONObj& query, const BSONObj& sort, const BSONObj& proj);

    void runInvalidQuerySortProjSkipNToReturn(const BSONObj& query,
                                              const BSONObj& sort,
                                              const BSONObj& proj,
                                              long long skip,
                                              long long ntoreturn);

    void runInvalidQueryHint(const BSONObj& query, const BSONObj& hint);

    void runInvalidQueryHintMinMax(const BSONObj& query,
                                   const BSONObj& hint,
                                   const BSONObj& minObj,
                                   const BSONObj& maxObj);

    void runInvalidQuerySortProjSkipNToReturnHint(const BSONObj& query,
                                                  const BSONObj& sort,
                                                  const BSONObj& proj,
                                                  long long skip,
                                                  long long ntoreturn,
                                                  const BSONObj& hint);

    void runInvalidQueryFull(const BSONObj& query,
                             const BSONObj& sort,
                             const BSONObj& proj,
                             long long skip,
                             long long ntoreturn,
                             const BSONObj& hint,
                             const BSONObj& minObj,
                             const BSONObj& maxObj,
                             bool snapshot);

    /**
     * The other runQuery* methods run the query as through it is an OP_QUERY style find. This
     * version goes through find command parsing, and will be planned like a find command.
     */
    void runQueryAsCommand(const BSONObj& cmdObj);

    //
    // Introspect solutions.
    //

    size_t getNumSolutions() const;

    void dumpSolutions() const;

    void dumpSolutions(mongoutils::str::stream& ost) const;

    /**
     * Checks number solutions. Generates assertion message
     * containing solution dump if applicable.
     */
    void assertNumSolutions(size_t expectSolutions) const;

    size_t numSolutionMatches(const std::string& solnJson) const;

    /**
     * Verifies that the solution tree represented in json by 'solnJson' is
     * one of the solutions generated by QueryPlanner.
     *
     * The number of expected matches, 'numMatches', could be greater than
     * 1 if solutions differ only by the pattern of index tags on a filter.
     */
    void assertSolutionExists(const std::string& solnJson, size_t numMatches = 1) const;

    /**
     * Given a vector of string-based solution tree representations 'solnStrs',
     * verifies that the query planner generated exactly one of these solutions.
     */
    void assertHasOneSolutionOf(const std::vector<std::string>& solnStrs) const;

    /**
     * Helper function to parse a MatchExpression.
     */
    static std::unique_ptr<MatchExpression> parseMatchExpression(
        const BSONObj& obj, const CollatorInterface* collator = nullptr);

    //
    // Data members.
    //

    static const NamespaceString nss;

    QueryTestServiceContext serviceContext;
    ServiceContext::UniqueOperationContext opCtx;
    BSONObj queryObj;
    std::unique_ptr<CanonicalQuery> cq;
    QueryPlannerParams params;
    OwnedPointerVector<QuerySolution> solns;
};

}  // namespace mongo
