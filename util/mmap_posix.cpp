// mmap_posix.cpp

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
#include "file_allocator.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace mongo {

    MemoryMappedFile::MemoryMappedFile() {
        fd = 0;
        maphandle = 0;
        view = 0;
        len = 0;
        created();
    }

    void MemoryMappedFile::close() {
        if ( view )
            munmap(view, len);
        view = 0;

        if ( fd )
            ::close(fd);
        fd = 0;
    }

#ifndef O_NOATIME
#define O_NOATIME 0
#endif

    void* MemoryMappedFile::map(const char *filename, int &length) {
        // length may be updated by callee.
        theFileAllocator().allocateAsap( filename, length );
        len = length;
        
        fd = open(filename, O_RDWR | O_NOATIME);
        if ( fd <= 0 ) {
            out() << "couldn't open " << filename << ' ' << errno << endl;
            return 0;
        }

        off_t filelen = lseek(fd, 0, SEEK_END);
        massert( "file size allocation failed", filelen == length );
        lseek( fd, 0, SEEK_SET );
        
        view = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if ( view == MAP_FAILED ) {
            out() << "  mmap() failed for " << filename << " len:" << length << " errno:" << errno << endl;
            return 0;
        }
        return view;
    }

    void MemoryMappedFile::flush(bool sync) {
        if ( msync(view, len, sync ? MS_SYNC : MS_ASYNC) )
            problem() << "msync error " << errno << endl;
    }
    

} // namespace mongo

