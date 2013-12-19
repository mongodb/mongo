/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/runner.h"

namespace mongo {

    /**
     * Get a runner for a query.  Takes ownership of rawCanonicalQuery.
     *
     * If the query is valid and a runner could be created, returns Status::OK()
     * and populates *out with the Runner.
     *
     * If the query cannot be executed, returns a Status indicating why.  Deletes
     * rawCanonicalQuery.
     */
    Status getRunner(CanonicalQuery* rawCanonicalQuery, Runner** out, size_t plannerOptions = 0);

    /**
     * RAII approach to ensuring that runners are deregistered in newRunQuery.
     *
     * While retrieving the first bach of results, newRunQuery manually registers the runner with
     * ClientCursor.  Certain query execution paths, namely $where, can throw an exception.  If we
     * fail to deregister the runner, we will call invalidate/kill on the
     * still-registered-yet-deleted runner.
     *
     * For any subsequent calls to getMore, the runner is already registered with ClientCursor
     * by virtue of being cached, so this exception-proofing is not required.
     */
    struct ScopedRunnerRegistration {
        ScopedRunnerRegistration(Runner* runner);
        ~ScopedRunnerRegistration();

        Runner* const _runner;
    };

}  // namespace mongo
