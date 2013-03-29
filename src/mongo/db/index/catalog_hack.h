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

#include "mongo/db/keypattern.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

    class CatalogHack {
    public:
        static IndexDescriptor* getDescriptor(IndexDetails &id) {
            return new IndexDescriptor(&id, id.info.obj());
        }

        // The IndexDetails passed in might refer to a Btree-backed index that is not a proper Btree
        // index.  Each Btree-backed index uses a BtreeCursor.  The BtreeCursor doesn't want the AM
        // for the backed index; it wants to talk Btree directly.  So BtreeCursor always asks for a
        // Btree index.
        static IndexAccessMethod* getBtreeIndex(IndexDescriptor* desc) {
            if (0 == desc->version()) {
                return new BtreeAccessMethod<V0>(desc);
            } else if (1 == desc->version()) {
                return new BtreeAccessMethod<V1>(desc);
            } else {
                verify(0);
                return NULL;
            }
        }
    };

}  // namespace mongo
