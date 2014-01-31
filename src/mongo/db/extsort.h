// extsort.h

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

#include "mongo/db/jsobj.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

    typedef pair<BSONObj, DiskLoc> ExternalSortDatum;

    /**
     * To external sort, you provide a pointer to an implementation of this class.
     * The compare function follows the usual -1, 0, 1 semantics.
     */
    class ExternalSortComparison {
    public:
        virtual ~ExternalSortComparison() { }
        virtual int compare(const ExternalSortDatum& l, const ExternalSortDatum& r) const = 0;
    };

    // TODO This class will probably disappear in the future or be replaced with a typedef
    class BSONObjExternalSorter : boost::noncopyable {
    public:
        typedef pair<BSONObj, DiskLoc> Data;
        typedef SortIteratorInterface<BSONObj, DiskLoc> Iterator;

        BSONObjExternalSorter(const ExternalSortComparison* comp, long maxFileSize=100*1024*1024);

        void add( const BSONObj& o, const DiskLoc& loc, bool mayInterrupt ) {
            *_mayInterrupt = mayInterrupt;
            _sorter->add(o.getOwned(), loc);
        }

        auto_ptr<Iterator> iterator() { return auto_ptr<Iterator>(_sorter->done()); }

        void sort( bool mayInterrupt ) { *_mayInterrupt = mayInterrupt; }
        int numFiles() { return _sorter->numFiles(); }
        long getCurSizeSoFar() { return _sorter->memUsed(); }
        void hintNumObjects(long long) {} // unused

    private:
        shared_ptr<bool> _mayInterrupt;
        scoped_ptr<Sorter<BSONObj, DiskLoc> > _sorter;
    };
}
