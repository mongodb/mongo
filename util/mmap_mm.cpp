// mmap_mm.cpp

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

#include "stdafx.h"
#include "mmap.h"

namespace mongo {

    MemoryMappedFile::MemoryMappedFile() {
        fd = 0;
        maphandle = 0;
        view = 0;
        len = 0;
    }

    void MemoryMappedFile::close() {
        if ( view )
            delete( view );
        view = 0;
        len = 0;
    }

    void* MemoryMappedFile::map(const char *filename, int length) {
        path p( filename );

        view = malloc( length );
        return view;
    }

    void MemoryMappedFile::flush(bool sync) {
    }
    

} 

