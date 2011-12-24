// compact.h

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
*/

#pragma once

namespace mongo {

    /** for bottom up fastbuildindex (where we presort keys) */
    struct SortPhaseOne { 
        SortPhaseOne() { 
            n = 0;
            nkeys = 0;
            multi = false;
        }
        shared_ptr<BSONObjExternalSorter> sorter;
        unsigned long long n; // # of records
        unsigned long long nkeys;
        bool multi; // multikey index

        void addKeys(const IndexSpec& spec, const BSONObj& o, DiskLoc loc) { 
            BSONObjSet keys;
            spec.getKeys(o, keys);
            int k = 0;
            for ( BSONObjSet::iterator i=keys.begin(); i != keys.end(); i++ ) {
                if( ++k == 2 ) {
                    multi = true;
                }
                sorter->add(*i, loc);
                nkeys++;
            }
            n++;
        }
    };

}
