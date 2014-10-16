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

#pragma once

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

    class OperationContext;

    /**
     * The internal planner is a one-stop shop for "off-the-shelf" plans.  Most internal procedures
     * that do not require advanced queries could be served by plans already in here.
     */
    class InternalPlanner {
    public:
        enum Direction {
            FORWARD = 1,
            BACKWARD = -1,
        };

        enum IndexScanOptions {
            // The client is interested in the default outputs of an index scan: BSONObj of the key,
            // DiskLoc of the record that's indexed.  The client does its own fetching if required.
            IXSCAN_DEFAULT = 0,

            // The client wants the fetched object and the DiskLoc that refers to it.  Delegating
            // the fetch to the runner allows fetching outside of a lock.
            IXSCAN_FETCH = 1,
        };

        /**
         * Return a collection scan.  Caller owns pointer.
         */
        static PlanExecutor* collectionScan(OperationContext* txn,
                                            const StringData& ns,
                                            Collection* collection,
                                            const Direction direction = FORWARD,
                                            const DiskLoc startLoc = DiskLoc()) {
            WorkingSet* ws = new WorkingSet();

            if (NULL == collection) {
                EOFStage* eof = new EOFStage();
                return new PlanExecutor(txn, ws, eof, ns.toString());
            }

            dassert( ns == collection->ns().ns() );

            CollectionScanParams params;
            params.collection = collection;
            params.start = startLoc;

            if (FORWARD == direction) {
                params.direction = CollectionScanParams::FORWARD;
            }
            else {
                params.direction = CollectionScanParams::BACKWARD;
            }

            CollectionScan* cs = new CollectionScan(txn, params, ws, NULL);
            // Takes ownership of 'ws' and 'cs'.
            return new PlanExecutor(txn, ws, cs, collection);
        }

        /**
         * Return an index scan.  Caller owns returned pointer.
         */
        static PlanExecutor* indexScan(OperationContext* txn,
                                       const Collection* collection,
                                       const IndexDescriptor* descriptor,
                                       const BSONObj& startKey, const BSONObj& endKey,
                                       bool endKeyInclusive, Direction direction = FORWARD,
                                       int options = 0) {
            invariant(collection);
            invariant(descriptor);

            IndexScanParams params;
            params.descriptor = descriptor;
            params.direction = direction;
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = startKey;
            params.bounds.endKey = endKey;
            params.bounds.endKeyInclusive = endKeyInclusive;

            WorkingSet* ws = new WorkingSet();
            IndexScan* ix = new IndexScan(txn, params, ws, NULL);

            PlanStage* root = ix;

            if (IXSCAN_FETCH & options) {
                root = new FetchStage(txn, ws, root, NULL, collection);
            }

            return new PlanExecutor(txn, ws, root, collection);
        }
    };

}  // namespace mongo
