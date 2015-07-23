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

#include "mongo/db/query/query_knobs.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"

namespace mongo {

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlanEvaluationWorks, int, 10000);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlanEvaluationCollFraction, double, 0.3);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlanEvaluationMaxResults, int, 101);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryCacheSize, int, 5000);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryCacheFeedbacksStored, int, 20);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryCacheEvictionRatio, double, 10.0);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlannerMaxIndexedSolutions, int, 64);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryEnumerationMaxOrSolutions, int, 10);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryEnumerationMaxIntersectPerAnd, int, 3);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryForceIntersectionPlans, bool, false);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlannerEnableIndexIntersection, bool, true);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlannerEnableHashIntersection, bool, false);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlanOrChildrenIndependently, bool, true);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryMaxScansToExplode, int, 200);

MONGO_EXPORT_SERVER_PARAMETER(internalQueryExecMaxBlockingSortBytes, int, 32 * 1024 * 1024);

// Yield every 128 cycles or 10ms.
MONGO_EXPORT_SERVER_PARAMETER(internalQueryExecYieldIterations, int, 128);
MONGO_EXPORT_SERVER_PARAMETER(internalQueryExecYieldPeriodMS, int, 10);

}  // namespace mongo
