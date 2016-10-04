// extent_manager.cpp

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

#include "mongo/db/storage/mmap_v1/extent_manager.h"

#include "mongo/db/storage/mmap_v1/extent.h"

namespace mongo {

int ExtentManager::quantizeExtentSize(int size) const {
    if (size == maxSize()) {
        // no point doing quantizing for the entire file
        return size;
    }

    invariant(size <= maxSize());

    // make sizes align with VM page size
    int newSize = (size + 0xfff) & 0xfffff000;

    if (newSize > maxSize()) {
        return maxSize();
    }

    if (newSize < minSize()) {
        return minSize();
    }

    return newSize;
}

int ExtentManager::followupSize(int len, int lastExtentLen) const {
    invariant(len < maxSize());
    int x = initialSize(len);
    // changed from 1.20 to 1.35 in v2.1.x to get to larger extent size faster
    int y = (int)(lastExtentLen < 4000000 ? lastExtentLen * 4.0 : lastExtentLen * 1.35);
    int sz = y > x ? y : x;

    if (sz < lastExtentLen) {
        // this means there was an int overflow
        // so we should turn it into maxSize
        return maxSize();
    } else if (sz > maxSize()) {
        return maxSize();
    }

    sz = quantizeExtentSize(sz);
    verify(sz >= len);

    return sz;
}

int ExtentManager::initialSize(int len) const {
    invariant(len <= maxSize());

    long long sz = len * 16;
    if (len < 1000)
        sz = len * 64;

    if (sz >= maxSize())
        return maxSize();

    if (sz <= minSize())
        return minSize();

    int z = ExtentManager::quantizeExtentSize(sz);
    verify(z >= len);
    return z;
}
}
