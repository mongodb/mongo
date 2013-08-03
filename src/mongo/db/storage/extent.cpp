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
*/

#include "mongo/pch.h"

#include "mongo/db/storage/extent.h"

#include "mongo/db/dur.h"
#include "mongo/util/mongoutils/str.h"

// XXX-ERH
#include "mongo/db/pdfile.h"

namespace mongo {

    static void extent_getEmptyLoc(const char *ns,
                                   const DiskLoc extentLoc,
                                   int extentLength,
                                   bool capped,
                                   /*out*/DiskLoc& emptyLoc,
                                   /*out*/int& delRecLength) {
        emptyLoc = extentLoc;
        emptyLoc.inc( Extent::HeaderSize() );
        delRecLength = extentLength - Extent::HeaderSize();
        if( delRecLength >= 32*1024 && str::contains(ns, '$') && !capped ) {
            // probably an index. so skip forward to keep its records page aligned
            int& ofs = emptyLoc.GETOFS();
            int newOfs = (ofs + 0xfff) & ~0xfff;
            delRecLength -= (newOfs-ofs);
            dassert( delRecLength > 0 );
            ofs = newOfs;
        }
    }


    BSONObj Extent::dump() {
        return BSON( "loc" << myLoc.toString()
                     << "xnext" << xnext.toString()
                     << "xprev" << xprev.toString()
                     << "nsdiag" << nsDiagnostic.toString()
                     << "size" << length
                     << "firstRecord"
                     << firstRecord.toString()
                     << "lastRecord" << lastRecord.toString() );
    }

    void Extent::dump(iostream& s) {
        s << "    loc:" << myLoc.toString()
          << " xnext:" << xnext.toString()
          << " xprev:" << xprev.toString() << '\n';
        s << "    nsdiag:" << nsDiagnostic.toString() << '\n';
        s << "    size:" << length
          << " firstRecord:" << firstRecord.toString()
          << " lastRecord:" << lastRecord.toString() << '\n';
    }

    void Extent::markEmpty() {
        xnext.Null();
        xprev.Null();
        firstRecord.Null();
        lastRecord.Null();
    }

    DiskLoc Extent::reuse(const char *nsname, bool capped) {
        return getDur().writing(this)->_reuse(nsname, capped);
    }

    DiskLoc Extent::_reuse(const char *nsname, bool capped) {
        LOG(3) << "_reuse extent was:" << nsDiagnostic.toString() << " now:" << nsname << endl;
        if (magic != extentSignature) {
            StringBuilder sb;
            sb << "bad extent signature " << integerToHex(magic)
               << " for namespace '" << nsDiagnostic.toString()
               << "' found in Extent::_reuse";
            msgasserted(10360, sb.str());
        }
        nsDiagnostic = nsname;
        markEmpty();

        DiskLoc emptyLoc;
        int delRecLength;
        extent_getEmptyLoc(nsname, myLoc, length, capped, emptyLoc, delRecLength);

        // todo: some dup code here and below in Extent::init
        DeletedRecord* empty = getDur().writing(DataFileMgr::getDeletedRecord(emptyLoc));
        empty->lengthWithHeaders() = delRecLength;
        empty->extentOfs() = myLoc.getOfs();
        empty->nextDeleted().Null();
        return emptyLoc;
    }

    /* assumes already zeroed -- insufficient for block 'reuse' perhaps */
    DiskLoc Extent::init(const char *nsname, int _length, int _fileNo, int _offset, bool capped) {
        magic = extentSignature;
        myLoc.set(_fileNo, _offset);
        xnext.Null();
        xprev.Null();
        nsDiagnostic = nsname;
        length = _length;
        firstRecord.Null();
        lastRecord.Null();

        DiskLoc emptyLoc;
        int delRecLength;
        extent_getEmptyLoc(nsname, myLoc, _length, capped, emptyLoc, delRecLength);

        DeletedRecord* empty = getDur().writing(DataFileMgr::getDeletedRecord(emptyLoc));
        empty->lengthWithHeaders() = delRecLength;
        empty->extentOfs() = myLoc.getOfs();
        empty->nextDeleted().Null();
        return emptyLoc;
    }

    bool Extent::validates(const DiskLoc diskLoc, BSONArrayBuilder* errors) {
        bool extentOk = true;
        if (magic != extentSignature) {
            if (errors) {
                StringBuilder sb;
                sb << "bad extent signature " << integerToHex(magic)
                    << " in extent " << diskLoc.toString();
                *errors << sb.str();
            }
            extentOk = false;
        }
        if (myLoc != diskLoc) {
            if (errors) {
                StringBuilder sb;
                sb << "extent " << diskLoc.toString()
                    << " self-pointer is " << myLoc.toString();
                *errors << sb.str();
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
                *errors << sb.str();
            }
            extentOk = false;
        }
        if (length < minSize()) {
            if (errors) {
                StringBuilder sb;
                sb << "length of extent " << diskLoc.toString()
                    << " is " << length
                    << ", which is less than minimum length of " << minSize();
                *errors << sb.str();
            }
            extentOk = false;
        }
        return extentOk;
    }

    int Extent::maxSize() {
        return DataFile::maxSize() - DataFileHeader::HeaderSize - 16;
    }


}
