/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kQuery

#include "merizo/platform/basic.h"

#include "merizo/db/index/haystack_access_method.h"


#include "merizo/base/status.h"
#include "merizo/db/bson/dotted_path_support.h"
#include "merizo/db/catalog/index_catalog.h"
#include "merizo/db/exec/working_set_common.h"
#include "merizo/db/geo/hash.h"
#include "merizo/db/index/expression_keys_private.h"
#include "merizo/db/index/expression_params.h"
#include "merizo/db/index/haystack_access_method_internal.h"
#include "merizo/db/jsobj.h"
#include "merizo/db/query/internal_plans.h"
#include "merizo/util/log.h"

namespace merizo {

using std::unique_ptr;

namespace dps = ::merizo::dotted_path_support;

HaystackAccessMethod::HaystackAccessMethod(IndexCatalogEntry* btreeState,
                                           SortedDataInterface* btree)
    : AbstractIndexAccessMethod(btreeState, btree) {
    const IndexDescriptor* descriptor = btreeState->descriptor();

    ExpressionParams::parseHaystackParams(
        descriptor->infoObj(), &_geoField, &_otherFields, &_bucketSize);

    uassert(16773, "no geo field specified", _geoField.size());
    uassert(16774, "no non-geo fields specified", _otherFields.size());
}

void HaystackAccessMethod::doGetKeys(const BSONObj& obj,
                                     BSONObjSet* keys,
                                     BSONObjSet* multikeyMetadataKeys,
                                     MultikeyPaths* multikeyPaths) const {
    ExpressionKeysPrivate::getHaystackKeys(obj, _geoField, _otherFields, _bucketSize, keys);
}

void HaystackAccessMethod::searchCommand(OperationContext* opCtx,
                                         Collection* collection,
                                         const BSONObj& nearObj,
                                         double maxDistance,
                                         const BSONObj& search,
                                         BSONObjBuilder* result,
                                         unsigned limit) const {
    Timer t;

    LOG(1) << "SEARCH near:" << redact(nearObj) << " maxDistance:" << maxDistance
           << " search: " << redact(search);
    int x, y;
    {
        BSONObjIterator i(nearObj);
        x = ExpressionKeysPrivate::hashHaystackElement(i.next(), _bucketSize);
        y = ExpressionKeysPrivate::hashHaystackElement(i.next(), _bucketSize);
    }
    int scale = static_cast<int>(ceil(maxDistance / _bucketSize));

    GeoHaystackSearchHopper hopper(opCtx, nearObj, maxDistance, limit, _geoField, collection);

    long long btreeMatches = 0;

    for (int a = -scale; a <= scale && !hopper.limitReached(); ++a) {
        for (int b = -scale; b <= scale && !hopper.limitReached(); ++b) {
            BSONObjBuilder bb;
            bb.append("", ExpressionKeysPrivate::makeHaystackString(x + a, y + b));

            for (unsigned i = 0; i < _otherFields.size(); i++) {
                // See if the non-geo field we're indexing on is in the provided search term.
                BSONElement e = dps::extractElementAtPath(search, _otherFields[i]);
                if (e.eoo())
                    bb.appendNull("");
                else
                    bb.appendAs(e, "");
            }

            BSONObj key = bb.obj();

            stdx::unordered_set<RecordId, RecordId::Hasher> thisPass;


            auto exec = InternalPlanner::indexScan(opCtx,
                                                   collection,
                                                   _descriptor,
                                                   key,
                                                   key,
                                                   BoundInclusion::kIncludeBothStartAndEndKeys,
                                                   PlanExecutor::NO_YIELD);
            PlanExecutor::ExecState state;
            BSONObj obj;
            RecordId loc;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, &loc))) {
                if (hopper.limitReached()) {
                    break;
                }
                pair<stdx::unordered_set<RecordId, RecordId::Hasher>::iterator, bool> p =
                    thisPass.insert(loc);
                // If a new element was inserted (haven't seen the RecordId before), p.second
                // is true.
                if (p.second) {
                    hopper.consider(loc);
                    btreeMatches++;
                }
            }

            // Non-yielding collection scans from InternalPlanner will never error.
            invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state);
        }
    }

    BSONArrayBuilder arr(result->subarrayStart("results"));
    int num = hopper.appendResultsTo(&arr);
    arr.done();

    {
        BSONObjBuilder b(result->subobjStart("stats"));
        b.append("time", t.millis());
        b.appendNumber("btreeMatches", btreeMatches);
        b.append("n", num);
        b.done();
    }
}

}  // namespace merizo
