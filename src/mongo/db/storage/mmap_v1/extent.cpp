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

#include "mongo/db/storage/mmap_v1/extent.h"

#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/util/hex.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::iostream;
using std::string;
using std::vector;

static_assert(sizeof(Extent) - 4 == 48 + 128, "sizeof(Extent) - 4 == 48 + 128");

BSONObj Extent::dump() const {
    return BSON("loc" << myLoc.toString() << "xnext" << xnext.toString() << "xprev"
                      << xprev.toString()
                      << "nsdiag"
                      << nsDiagnostic.toString()
                      << "size"
                      << length
                      << "firstRecord"
                      << firstRecord.toString()
                      << "lastRecord"
                      << lastRecord.toString());
}

void Extent::dump(iostream& s) const {
    s << "    loc:" << myLoc.toString() << " xnext:" << xnext.toString()
      << " xprev:" << xprev.toString() << '\n';
    s << "    nsdiag:" << nsDiagnostic.toString() << '\n';
    s << "    size:" << length << " firstRecord:" << firstRecord.toString()
      << " lastRecord:" << lastRecord.toString() << '\n';
}

bool Extent::validates(const DiskLoc diskLoc, vector<string>* errors) const {
    bool extentOk = true;
    if (magic != extentSignature) {
        if (errors) {
            StringBuilder sb;
            sb << "bad extent signature " << integerToHex(magic) << " in extent "
               << diskLoc.toString();
            errors->push_back(sb.str());
        }
        extentOk = false;
    }
    if (myLoc != diskLoc) {
        if (errors) {
            StringBuilder sb;
            sb << "extent " << diskLoc.toString() << " self-pointer is " << myLoc.toString();
            errors->push_back(sb.str());
        }
        extentOk = false;
    }
    if (firstRecord.isNull() != lastRecord.isNull()) {
        if (errors) {
            StringBuilder sb;
            if (firstRecord.isNull()) {
                sb << "in extent " << diskLoc.toString()
                   << ", firstRecord is null but lastRecord is " << lastRecord.toString();
            } else {
                sb << "in extent " << diskLoc.toString() << ", firstRecord is "
                   << firstRecord.toString() << " but lastRecord is null";
            }
            errors->push_back(sb.str());
        }
        extentOk = false;
    }
    static const int minSize = 0x1000;
    if (length < minSize) {
        if (errors) {
            StringBuilder sb;
            sb << "length of extent " << diskLoc.toString() << " is " << length
               << ", which is less than minimum length of " << minSize;
            errors->push_back(sb.str());
        }
        extentOk = false;
    }
    return extentOk;
}
}
