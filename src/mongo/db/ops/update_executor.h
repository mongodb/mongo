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

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

    class CanonicalQuery;
    class OpDebug;
    class OperationContext;
    class UpdateRequest;

    /**
     * Implementation of the processing of an update operation in a mongod.
     *
     * The executor has two important methods, prepare() and execute().  The prepare() method can
     * run without locks, and does whatever parsing and precomputation can be done without access to
     * database data.  The execute method performs the update, but the caller must already hold the
     * appropriate database lock.
     *
     * Expected usage is approximately:
     *   UpdateRequest request(...);
     *   // configure request
     *   UpdateExecutor executor(txn, &request, opDebug);
     *   uassertStatusOK(executor.prepare());
     *   // Get locks, get ready to execute.
     *   try {
     *       UpdateResult res = executor.execute();
     *   }
     *   catch (const DBException& ex) {
     *       // Error handling.
     *   }
     */
    class UpdateExecutor {
        MONGO_DISALLOW_COPYING(UpdateExecutor);
    public:
        /**
         * Constructs an update executor.
         *
         * The objects pointed to by "request" and "opDebug" must stay in scope for the life of the
         * constructed executor.
         */
        UpdateExecutor(OperationContext* txn, const UpdateRequest* request, OpDebug* opDebug);

        ~UpdateExecutor();

        /**
         * Performs preparatory work that does not require database locks.
         *
         * Returns Status::OK() on success.  Other results indicate that the executor will not run
         * correctly, and should be abandoned.
         *
         * Calling prepare() is optional.  It is available for situations in which the user
         * wishes to do as much work as possible before acquiring database locks.
         */
        Status prepare();

        /**
         * Performs preparatory work that *does* require the appropriate database lock. This
         * preparation involves construction of a PlanExecutor. Construction of a PlanExecutor
         * requires the database lock because it goes through query planning and optimization,
         * which may involve partial execution of the update plan tree.
         *
         * On success, a non-NULL PlanExecutor will be available via getPlanExecutor().
         */
        Status prepareInLock(Database* db);

        /**
         * Retrieve the PlanExecutor that will be used to execute this update upon calling
         * execute(). Returns NULL if no PlanExecutor has been created.
         */
        PlanExecutor* getPlanExecutor();

        /**
         * Execute an update.  Requires the caller to hold the database lock on the
         * appropriate resources for the request.
         */
        UpdateResult execute(Database* db);

    private:
        /**
         * Parses the query portion of the update request.
         */
        Status parseQuery();

        /**
         * Parses the update-descriptor portion of the update request.
         */
        Status parseUpdate();

        // Transactional context.  Not owned by us.
        OperationContext* _txn;

        // Unowned pointer to the request object that this executor will process.
        const UpdateRequest* const _request;

        // Unowned pointer to the opdebug object that this executor will populate with debug data.
        OpDebug* const _opDebug;

        // Driver for processing updates on matched documents.
        UpdateDriver _driver;

        // Parsed query object, or NULL if the query proves to be an id hack query.
        std::auto_ptr<CanonicalQuery> _canonicalQuery;

        // The tree of execution stages which will be used to execute the update.
        boost::scoped_ptr<PlanExecutor> _exec;

        // Flag indicating if the query has been successfully parsed.
        bool _isQueryParsed;

        // Flag indicatin gif the update description has been successfully parsed.
        bool _isUpdateParsed;
    };

}  // namespace mongo
