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
 */

#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/query/eof_runner.h"
#include "mongo/db/query/internal_runner.h"

namespace mongo {

    /**
     * The internal planner is a one-stop shop for "off-the-shelf" plans.  Most internal procedures
     * that do not require advanced queries could be served by plans already in here.
     */
    class InternalPlanner {
    public:
        /**
         * A collection scan.  Caller owns pointer.
         */
        static Runner* findAll(const StringData& ns, const DiskLoc startLoc = DiskLoc()) {
            NamespaceDetails* nsd = nsdetails(ns);
            if (NULL == nsd) { return new EOFRunner(ns.toString()); }

            CollectionScanParams params;
            params.ns = ns.toString();
            params.start = startLoc;
            WorkingSet* ws = new WorkingSet();
            CollectionScan* cs = new CollectionScan(params, ws, NULL);

            return new InternalRunner(ns.toString(), cs, ws);
        }
    };

}  // namespace mongo
