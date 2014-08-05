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

#include "mongo/db/storage/heap1/heap1_database_catalog_entry.h"

#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/storage/heap1/heap1_btree_impl.h"

namespace mongo {
    IndexAccessMethod* Heap1DatabaseCatalogEntry::getIndex( OperationContext* txn,
                                                            const CollectionCatalogEntry* collection,
                                                            IndexCatalogEntry* index ) {
        const Entry* entry = dynamic_cast<const Entry*>( collection );

        Entry::Indexes::const_iterator i = entry->indexes.find( index->descriptor()->indexName() );
        if ( i == entry->indexes.end() ) {
            // index doesn't exist
            return NULL;
        }

        const string& type = index->descriptor()->getAccessMethodName();

#if 1 // Toggle to use Btree on HeapRecordStore

        // Need the Head to be non-Null to avoid asserts. TODO remove the asserts.
        index->headManager()->setHead(txn, DiskLoc(0xDEAD, 0xBEAF));

        // When is a btree not a Btree? When it is a Heap1BtreeImpl!
        std::auto_ptr<SortedDataInterface> btree(getHeap1BtreeImpl(index, &i->second->data));
#else

        if (!i->second->rs)
            i->second->rs.reset(new HeapRecordStore( index->descriptor()->indexName() ));

        std::auto_ptr<SortedDataInterface> btree(
            SortedDataInterface::getInterface(index->headManager(),
                                         i->second->rs,
                                         index->ordering(),
                                         index->descriptor()->indexNamespace(),
                                         index->descriptor()->version(),
                                         &BtreeBasedAccessMethod::invalidateCursors));
#endif

        if ("" == type)
            return new BtreeAccessMethod( index, btree.release() );

        if (IndexNames::HASHED == type)
            return new HashAccessMethod( index, btree.release() );

        if (IndexNames::GEO_2DSPHERE == type)
            return new S2AccessMethod( index, btree.release() );

        if (IndexNames::TEXT == type)
            return new FTSAccessMethod( index, btree.release() );

        if (IndexNames::GEO_HAYSTACK == type)
            return new HaystackAccessMethod( index, btree.release() );

        if (IndexNames::GEO_2D == type)
            return new TwoDAccessMethod( index, btree.release() );

        log() << "Can't find index for keyPattern " << index->descriptor()->keyPattern();
        fassertFailed(18518);
    }
}
