/* hashtab.h

   Simple, fixed size hash table.  Darn simple.

   Uses a contiguous block of memory, so you can put it in a memory mapped file very easily.
*/

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <map>

#include "mongo/db/storage/mmap_v1/catalog/namespace.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/functional.h"

namespace mongo {

#pragma pack(1)

    /* you should define:

       int Key::hash() return > 0 always.
       Used in NamespaceIndex only.
    */

    class NamespaceHashTable : boost::noncopyable {
    public:
        typedef Namespace Key;
        typedef NamespaceDetails Type;

        const char *name;
        struct Node {
            int hash;
            Key k;
            Type value;
            bool inUse() {
                return hash != 0;
            }
            void setUnused() {
                hash = 0;
            }
        };
        void* _buf;
        int n; // number of hashtable buckets
        int maxChain;

        Node& nodes(int i) {
            Node *nodes = (Node *) _buf;
            return nodes[i];
        }

        int _find(const Key& k, bool& found);

    public:
        /* buf must be all zeroes on initialization. */
        NamespaceHashTable(void* buf, int buflen, const char *_name);

        Type* get(const Key& k) {
            bool found;
            int i = _find(k, found);
            if ( found )
                return &nodes(i).value;
            return 0;
        }

        void kill(OperationContext* txn, const Key& k) {
            bool found;
            int i = _find(k, found);
            if ( i >= 0 && found ) {
                Node* n = &nodes(i);
                n = txn->recoveryUnit()->writing(n);
                n->k.kill();
                n->setUnused();
            }
        }

        /** returns false if too full */
        bool put(OperationContext* txn, const Key& k, const Type& value) {
            bool found;
            int i = _find(k, found);
            if ( i < 0 )
                return false;
            Node* n = txn->recoveryUnit()->writing( &nodes(i) );
            if ( !found ) {
                n->k = k;
                n->hash = k.hash();
            }
            else {
                verify( n->hash == k.hash() );
            }
            n->value = value;
            return true;
        }

        typedef stdx::function< void ( const Key& k , Type& v ) > IteratorCallback;
        void iterAll( IteratorCallback callback ) {
            for ( int i=0; i<n; i++ ) {
                if ( nodes(i).inUse() ) {
                    callback( nodes(i).k , nodes(i).value );
                }
            }
        }

    };

#pragma pack()

} // namespace mongo
