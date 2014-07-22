/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/index/haystack_access_method.h"

#include "mongo/base/status.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/haystack_access_method_internal.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/util/log.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kQuery);

    HaystackAccessMethod::HaystackAccessMethod(IndexCatalogEntry* btreeState, SortedDataInterface* btree)
        : BtreeBasedAccessMethod(btreeState, btree) {

        const IndexDescriptor* descriptor = btreeState->descriptor();

        ExpressionParams::parseHaystackParams(descriptor->infoObj(),
                                              &_geoField,
                                              &_otherFields,
                                              &_bucketSize);

        uassert(16773, "no geo field specified", _geoField.size());
        uassert(16774, "no non-geo fields specified", _otherFields.size());
    }

    void HaystackAccessMethod::getKeys(const BSONObj& obj, BSONObjSet* keys) {
        ExpressionKeysPrivate::getHaystackKeys(obj, _geoField, _otherFields, _bucketSize, keys);
    }

    void HaystackAccessMethod::searchCommand(OperationContext* txn, Collection* collection,
                                             const BSONObj& nearObj, double maxDistance,
                                             const BSONObj& search, BSONObjBuilder* result,
                                             unsigned limit) {
        Timer t;

        LOG(1) << "SEARCH near:" << nearObj << " maxDistance:" << maxDistance
               << " search: " << search << endl;
        int x, y;
        {
            BSONObjIterator i(nearObj);
            x = ExpressionKeysPrivate::hashHaystackElement(i.next(), _bucketSize);
            y = ExpressionKeysPrivate::hashHaystackElement(i.next(), _bucketSize);
        }
        int scale = static_cast<int>(ceil(maxDistance / _bucketSize));

        GeoHaystackSearchHopper hopper(nearObj, maxDistance, limit, _geoField, collection);

        long long btreeMatches = 0;

        for (int a = -scale; a <= scale && !hopper.limitReached(); ++a) {
            for (int b = -scale; b <= scale && !hopper.limitReached(); ++b) {
                BSONObjBuilder bb;
                bb.append("", ExpressionKeysPrivate::makeHaystackString(x + a, y + b));

                for (unsigned i = 0; i < _otherFields.size(); i++) {
                    // See if the non-geo field we're indexing on is in the provided search term.
                    BSONElement e = search.getFieldDotted(_otherFields[i]);
                    if (e.eoo())
                        bb.appendNull("");
                    else
                        bb.appendAs(e, "");
                }

                BSONObj key = bb.obj();

                unordered_set<DiskLoc, DiskLoc::Hasher> thisPass;


                scoped_ptr<PlanExecutor> exec(InternalPlanner::indexScan(txn,  collection,
                                                                     _descriptor, key, key, true));
                PlanExecutor::ExecState state;
                DiskLoc loc;
                while (PlanExecutor::ADVANCED == (state = exec->getNext(NULL, &loc))) {
                    if (hopper.limitReached()) { break; }
                    pair<unordered_set<DiskLoc, DiskLoc::Hasher>::iterator, bool> p
                        = thisPass.insert(loc);
                    // If a new element was inserted (haven't seen the DiskLoc before), p.second
                    // is true.
                    if (p.second) {
                        hopper.consider(loc);
                        btreeMatches++;
                    }
                }
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

}  // namespace mongo
