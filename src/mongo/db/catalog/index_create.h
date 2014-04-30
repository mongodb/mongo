// index_create.h

/**
*    Copyright (C) 2008 10gen Inc.
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
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/index/index_access_method.h"

namespace mongo {

    class BSONObj;
    class Collection;
    class IndexCatalogEntry;
    class TransactionExperiment;

    // Build an index in the foreground
    // If background is false, uses fast index builder
    // If background is true, uses background index builder; blocks until done.
    void buildAnIndex( TransactionExperiment* txn,
                       Collection* collection,
                       IndexCatalogEntry* btreeState,
                       bool mayInterrupt );

    class MultiIndexBlock {
        MONGO_DISALLOW_COPYING( MultiIndexBlock );
    public:
        MultiIndexBlock(TransactionExperiment* txn,
                        Collection* collection );
        ~MultiIndexBlock();

        Status init( std::vector<BSONObj>& specs );

        Status insert( const BSONObj& doc,
                       const DiskLoc& loc,
                       const InsertDeleteOptions& options );

        Status commit();

    private:
        Collection* _collection;

        struct IndexState {
            IndexState()
                : block( NULL ), real( NULL ), bulk( NULL ) {
            }

            IndexAccessMethod* forInsert() { return bulk ? bulk : real; }

            IndexCatalog::IndexBuildBlock* block;
            IndexAccessMethod* real;
            IndexAccessMethod* bulk;
        };

        std::vector<IndexState> _states;

        // Not owned here, must outlive 'this'
        TransactionExperiment* _txn;
    };

} // namespace mongo
