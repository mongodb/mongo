// extent.cpp

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

#include "mongo/db/storage/extent.h"

#include "mongo/db/storage/data_file.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    BOOST_STATIC_ASSERT( sizeof(Extent)-4 == 48+128 );

    int Extent::initialSize(int len) {
        verify( len <= maxSize() );

        long long sz = len * 16;
        if ( len < 1000 )
            sz = len * 64;

        if ( sz >= maxSize() )
            return maxSize();

        if ( sz <= minSize() )
            return minSize();

        int z = ExtentManager::quantizeExtentSize( sz );
        verify( z >= len );
        return z;
    }

    int Extent::followupSize(int len, int lastExtentLen) {
        verify( len < Extent::maxSize() );
        int x = initialSize(len);
        // changed from 1.20 to 1.35 in v2.1.x to get to larger extent size faster
        int y = (int) (lastExtentLen < 4000000 ? lastExtentLen * 4.0 : lastExtentLen * 1.35);
        int sz = y > x ? y : x;

        if ( sz < lastExtentLen ) {
            // this means there was an int overflow
            // so we should turn it into maxSize
            return Extent::maxSize();
        }
        else if ( sz > Extent::maxSize() ) {
            return Extent::maxSize();
        }

        sz = ExtentManager::quantizeExtentSize( sz );
        verify( sz >= len );

        return sz;
    }

    BSONObj Extent::dump() const {
        return BSON( "loc" << myLoc.toString()
                     << "xnext" << xnext.toString()
                     << "xprev" << xprev.toString()
                     << "nsdiag" << nsDiagnostic.toString()
                     << "size" << length
                     << "firstRecord"
                     << firstRecord.toString()
                     << "lastRecord" << lastRecord.toString() );
    }

    void Extent::dump(iostream& s) const {
        s << "    loc:" << myLoc.toString()
          << " xnext:" << xnext.toString()
          << " xprev:" << xprev.toString() << '\n';
        s << "    nsdiag:" << nsDiagnostic.toString() << '\n';
        s << "    size:" << length
          << " firstRecord:" << firstRecord.toString()
          << " lastRecord:" << lastRecord.toString() << '\n';
    }

    bool Extent::validates(const DiskLoc diskLoc, vector<string>* errors) const {
        bool extentOk = true;
        if (magic != extentSignature) {
            if (errors) {
                StringBuilder sb;
                sb << "bad extent signature " << integerToHex(magic)
                    << " in extent " << diskLoc.toString();
                errors->push_back( sb.str() );
            }
            extentOk = false;
        }
        if (myLoc != diskLoc) {
            if (errors) {
                StringBuilder sb;
                sb << "extent " << diskLoc.toString()
                    << " self-pointer is " << myLoc.toString();
                errors->push_back( sb.str() );
            }
            extentOk = false;
        }
        if (firstRecord.isNull() != lastRecord.isNull()) {
            if (errors) {
                StringBuilder sb;
                if (firstRecord.isNull()) {
                    sb << "in extent " << diskLoc.toString()
                        << ", firstRecord is null but lastRecord is "
                        << lastRecord.toString();
                }
                else {
                    sb << "in extent " << diskLoc.toString()
                        << ", firstRecord is " << firstRecord.toString()
                        << " but lastRecord is null";
                }
                errors->push_back( sb.str() );
            }
            extentOk = false;
        }
        if (length < minSize()) {
            if (errors) {
                StringBuilder sb;
                sb << "length of extent " << diskLoc.toString()
                    << " is " << length
                    << ", which is less than minimum length of " << minSize();
                errors->push_back( sb.str() );
            }
            extentOk = false;
        }
        return extentOk;
    }

    int Extent::maxSize() {
        return DataFile::maxSize() - DataFileHeader::HeaderSize - 16;
    }


}
