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

#include "mongo/pch.h"
#include <map>
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/operation_context.h"

namespace mongo {

#pragma pack(1)

    /* you should define:

       int Key::hash() return > 0 always.
    */

    template <class Key,class Type>
    class HashTable : boost::noncopyable {
    public:
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

        int _find(const Key& k, bool& found) {
            found = false;
            int h = k.hash();
            int i = h % n;
            int start = i;
            int chain = 0;
            int firstNonUsed = -1;
            while ( 1 ) {
                if ( !nodes(i).inUse() ) {
                    if ( firstNonUsed < 0 )
                        firstNonUsed = i;
                }

                if ( nodes(i).hash == h && nodes(i).k == k ) {
                    if ( chain >= 200 )
                        log() << "warning: hashtable " << name << " long chain " << std::endl;
                    found = true;
                    return i;
                }
                chain++;
                i = (i+1) % n;
                if ( i == start ) {
                    // shouldn't get here / defensive for infinite loops
                    log() << "error: hashtable " << name << " is full n:" << n << std::endl;
                    return -1;
                }
                if( chain >= maxChain ) {
                    if ( firstNonUsed >= 0 )
                        return firstNonUsed;
                    log() << "error: hashtable " << name << " max chain reached:" << maxChain << std::endl;
                    return -1;
                }
            }
        }

    public:
        /* buf must be all zeroes on initialization. */
        HashTable(void* buf, int buflen, const char *_name) : name(_name) {
            int m = sizeof(Node);
            // log() << "hashtab init, buflen:" << buflen << " m:" << m << std::endl;
            n = buflen / m;
            if ( (n & 1) == 0 )
                n--;
            maxChain = (int) (n * 0.05);
            _buf = buf;
            //nodes = (Node *) buf;

            if ( sizeof(Node) != 628 ) {
                log() << "HashTable() " << _name << " sizeof(node):" << sizeof(Node) << " n:" << n << " sizeof(Key): " << sizeof(Key) << " sizeof(Type):" << sizeof(Type) << std::endl;
                verify( sizeof(Node) == 628 );
            }

        }

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

        typedef void (*IteratorCallback)( const Key& k , Type& v );
        void iterAll( IteratorCallback callback ) {
            for ( int i=0; i<n; i++ ) {
                if ( nodes(i).inUse() ) {
                    callback( nodes(i).k , nodes(i).value );
                }
            }
        }

        // TODO: should probably use stdx::bind for this, but didn't want to look at it
        typedef void (*IteratorCallback2)( const Key& k , Type& v , void * extra );
        void iterAll( IteratorCallback2 callback , void * extra ) {
            for ( int i=0; i<n; i++ ) {
                if ( nodes(i).inUse() ) {
                    callback( nodes(i).k , nodes(i).value , extra );
                }
            }
        }

    };

#pragma pack()

} // namespace mongo
