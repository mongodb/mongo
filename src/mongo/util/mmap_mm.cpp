// mmap_mm.cpp - in memory (no file) version

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/util/mmap.h"

#include "mongo/util/allocator.h"

/* in memory (no file) version */

namespace mongo {

    MemoryMappedFile::MemoryMappedFile() : _uniqueId(0) {
        fd = 0;
        maphandle = 0;
        view = 0;
        len = 0;
    }

    void MemoryMappedFile::close() {
        if ( view )
            free( view );
        view = 0;
        len = 0;
    }

    void* MemoryMappedFile::map(const char *filename, long& length , int options ) {
        verify( length );
        view = mongoMalloc( length );
        return view;
    }

    void MemoryMappedFile::flush(bool sync) {
    }

    void MemoryMappedFile::_lock() {}
    void MemoryMappedFile::_unlock() {}

}

