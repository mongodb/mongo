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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/catalog/hashtab.h"

#include "mongo/util/log.h"

namespace mongo {

int NamespaceHashTable::_find(const Namespace& k, bool& found) const {
    found = false;
    int h = k.hash();
    int i = h % n;
    int start = i;
    int chain = 0;
    int firstNonUsed = -1;
    while (1) {
        if (!_nodes(i).inUse()) {
            if (firstNonUsed < 0)
                firstNonUsed = i;
        }

        if (_nodes(i).hash == h && _nodes(i).key == k) {
            if (chain >= 200)
                log() << "warning: hashtable " << _name << " long chain " << std::endl;
            found = true;
            return i;
        }
        chain++;
        i = (i + 1) % n;
        if (i == start) {
            // shouldn't get here / defensive for infinite loops
            log() << "error: hashtable " << _name << " is full n:" << n << std::endl;
            return -1;
        }
        if (chain >= maxChain) {
            if (firstNonUsed >= 0)
                return firstNonUsed;
            log() << "error: hashtable " << _name << " max chain reached:" << maxChain << std::endl;
            return -1;
        }
    }
}

/* buf must be all zeroes on initialization. */
NamespaceHashTable::NamespaceHashTable(void* buf, int buflen, const char* name)
    : _name(name), _buf(buf) {
    n = buflen / sizeof(Node);
    if ((n & 1) == 0) {
        n--;
    }

    maxChain = (int)(n * 0.05);
}

}  // namespace mongo
