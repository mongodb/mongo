/**
 *    Copyright (C) 2012 10gen Inc.
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

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/background.h"

namespace mongo {

    class Collection;
    class Database;
    class OperationContext;

    /**
     * Forks off a thread to build an index.
     */
    class IndexBuilder : public BackgroundJob {
    public:
        IndexBuilder(const BSONObj& index);
        virtual ~IndexBuilder();

        virtual void run();

        /**
         * name of the builder, not the index
         */
        virtual std::string name() const;

        Status buildInForeground(OperationContext* txn, Database* db) const;

        /**
         * Kill all in-progress indexes matching criteria, if non-empty:
         * index ns, index name, and/or index key spec.
         * Returns a vector of the indexes that were killed.
         */
        static std::vector<BSONObj> 
            killMatchingIndexBuilds(Collection* collection,
                                    const IndexCatalog::IndexKillCriteria& criteria);

        /**
         * Retry all index builds in the list. Builds each index in a separate thread. If ns does
         * not match the ns field in the indexes list, the BSONObj's ns field is changed before the
         * index is built (to handle rename).
         */
        static void restoreIndexes(const std::vector<BSONObj>& indexes);

    private:
        Status _build(OperationContext* txn, Database* db, bool allowBackgroundBuilding) const;

        const BSONObj _index;
        std::string _name; // name of this builder, not related to the index
        static AtomicUInt32 _indexBuildCount;
    };

}
