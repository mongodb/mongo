// namespace_index.h

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

#include <list>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"

namespace mongo {

class NamespaceDetails;
class NamespaceHashTable;
class OperationContext;

/* NamespaceIndex is the ".ns" file you see in the data directory.  It is the "system catalog"
   if you will: at least the core parts.  (Additional info in system.* collections.)
*/
class NamespaceIndex {
    MONGO_DISALLOW_COPYING(NamespaceIndex);

public:
    NamespaceIndex(const std::string& dir, const std::string& database);
    ~NamespaceIndex();

    /* returns true if the file represented by this file exists on disk */
    bool pathExists() const;

    void init(OperationContext* txn);

    void add_ns(OperationContext* txn, StringData ns, const DiskLoc& loc, bool capped);
    void add_ns(OperationContext* txn, StringData ns, const NamespaceDetails* details);
    void add_ns(OperationContext* txn, const Namespace& ns, const NamespaceDetails* details);

    NamespaceDetails* details(StringData ns) const;
    NamespaceDetails* details(const Namespace& ns) const;

    void kill_ns(OperationContext* txn, StringData ns);

    bool allocated() const {
        return _ht.get() != 0;
    }

    void getCollectionNamespaces(std::list<std::string>* tofill) const;

    boost::filesystem::path path() const;

    unsigned long long fileLength() const {
        return _f.length();
    }

private:
    void maybeMkdir() const;

    const std::string _dir;
    const std::string _database;

    DurableMappedFile _f{MongoFile::Options::SEQUENTIAL};
    std::unique_ptr<NamespaceHashTable> _ht;
};
}
