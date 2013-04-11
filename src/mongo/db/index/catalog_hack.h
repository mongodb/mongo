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

#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/keypattern.h"

namespace mongo {

    /**
     * Fake some catalog behavior until the catalog is finalized.
     */
    class CatalogHack {
    public:
        static IndexDescriptor* getDescriptor(NamespaceDetails* nsd, int idxNo) {
            IndexDetails& id = nsd->idx(idxNo);
            return new IndexDescriptor(nsd, idxNo, &id, id.info.obj());
        }

        static bool isIndexMigrated(const BSONObj& keyPattern) {
            string type = KeyPattern::findPluginName(keyPattern);
            return "" == type || "fts" == type || "hashed" == type || "2dsphere" == type
                   || "geoHaystack" == type;
        }

        // If true, use EmulatedCursor + IndexCursor when answering "special" queries.
        static bool testCursorMigration() { return true; }

        // If true, use IndexAccessMethod for insert/delete when possible.
        static bool testIndexMigration() { return true; }

        static IndexAccessMethod* getIndex(IndexDescriptor* desc) {
            string type = KeyPattern::findPluginName(desc->keyPattern());
            if ("hashed" == type) {
                return new HashAccessMethod(desc);
            } else if ("2dsphere" == type) {
                return new S2AccessMethod(desc);
            } else if ("fts" == type) {
                return new FTSAccessMethod(desc);
            } else if ("geoHaystack" == type) {
                return new HaystackAccessMethod(desc);
            } else if ("" == type) {
                return new BtreeAccessMethod(desc);
            } else {
                verify(0);
                return NULL;
            }
        }

        // The IndexDetails passed in might refer to a Btree-backed index that is not a proper Btree
        // index.  Each Btree-backed index uses a BtreeCursor.  The BtreeCursor doesn't want the AM
        // for the backed index; it wants to talk Btree directly.  So BtreeCursor always asks for a
        // Btree index.
        static IndexAccessMethod* getBtreeIndex(IndexDescriptor* desc) {
            return new BtreeAccessMethod(desc);
        }
    };

}  // namespace mongo
