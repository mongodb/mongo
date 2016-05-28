/**
 *    Copyright (C) 2009-2014 MongoDB Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"
#include "mongo/stdx/functional.h"

namespace mongo {

/**
 * Simple, fixed size hash table used for namespace mapping (effectively the contents of the
 * MMAP V1 .ns file). Uses a contiguous block of memory, so you can put it in a memory mapped
 * file very easily.
 */
class NamespaceHashTable {
    MONGO_DISALLOW_COPYING(NamespaceHashTable);

public:
    typedef stdx::function<void(const Namespace& k, NamespaceDetails& v)> IteratorCallback;


    /* buf must be all zeroes on initialization. */
    NamespaceHashTable(void* buf, int buflen, const char* name);

    NamespaceDetails* get(const Namespace& k) const {
        bool found;
        int i = _find(k, found);
        if (found) {
            return &_nodes(i).value;
        }

        return 0;
    }

    void kill(OperationContext* txn, const Namespace& k) {
        bool found;
        int i = _find(k, found);
        if (i >= 0 && found) {
            Node* n = &_nodes(i);
            n = txn->recoveryUnit()->writing(n);
            n->key.kill();
            n->setUnused();
        }
    }

    /** returns false if too full */
    bool put(OperationContext* txn, const Namespace& k, const NamespaceDetails& value) {
        bool found;
        int i = _find(k, found);
        if (i < 0)
            return false;

        Node* n = txn->recoveryUnit()->writing(&_nodes(i));
        if (!found) {
            n->key = k;
            n->hash = k.hash();
        } else {
            invariant(n->hash == k.hash());
        }

        n->value = value;
        return true;
    }

    void iterAll(IteratorCallback callback) {
        for (int i = 0; i < n; i++) {
            if (_nodes(i).inUse()) {
                callback(_nodes(i).key, _nodes(i).value);
            }
        }
    }


private:
#pragma pack(1)
    struct Node {
        int hash;
        Namespace key;
        NamespaceDetails value;

        bool inUse() const {
            return hash != 0;
        }

        void setUnused() {
            hash = 0;
        }
    };
#pragma pack()

    static_assert(sizeof(Node) == 628, "sizeof(Node) == 628");


    int _find(const Namespace& k, bool& found) const;

    Node& _nodes(int i) const {
        Node* nodes = (Node*)_buf;
        return nodes[i];
    }


    const char* _name;
    void* const _buf;

    int n;  // number of hashtable buckets
    int maxChain;
};

}  // namespace mongo
