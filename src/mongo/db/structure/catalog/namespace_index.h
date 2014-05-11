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

#include "mongo/db/diskloc.h"
#include "mongo/db/structure/catalog/hashtab.h"
#include "mongo/db/structure/catalog/namespace.h"

namespace mongo {

    class NamespaceDetails;
    class TransactionExperiment;

    /* NamespaceIndex is the ".ns" file you see in the data directory.  It is the "system catalog"
       if you will: at least the core parts.  (Additional info in system.* collections.)
    */
    class NamespaceIndex {
    public:
        NamespaceIndex(const std::string &dir, const std::string &database) :
            _ht( 0 ), _dir( dir ), _database( database ) {}

        /* returns true if new db will be created if we init lazily */
        bool exists() const;

        void init( TransactionExperiment* txn ) {
            if ( !_ht.get() )
                _init( txn );
        }

        void add_ns( TransactionExperiment* txn,
                     const StringData& ns, const DiskLoc& loc, bool capped);
        void add_ns( TransactionExperiment* txn,
                     const StringData& ns, const NamespaceDetails* details );
        void add_ns( TransactionExperiment* txn,
                     const Namespace& ns, const NamespaceDetails* details );

        NamespaceDetails* details(const StringData& ns);
        NamespaceDetails* details(const Namespace& ns);

        void kill_ns( TransactionExperiment* txn,
                      const StringData& ns);

        bool allocated() const { return _ht.get() != 0; }

        void getNamespaces( std::list<std::string>& tofill , bool onlyCollections = true ) const;

        boost::filesystem::path path() const;

        unsigned long long fileLength() const { return _f.length(); }

    private:
        void _init( TransactionExperiment* txn );
        void maybeMkdir() const;

        DurableMappedFile _f;
        auto_ptr<HashTable<Namespace,NamespaceDetails> > _ht;
        std::string _dir;
        std::string _database;
    };

}
