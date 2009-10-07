// mmap.h

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

    class MemoryMappedFile {
    public:

        MemoryMappedFile();
        ~MemoryMappedFile(); /* closes the file if open */
        void close();
        
        // Throws exception if file doesn't exist.
        void* map( const char *filename );

        /* Creates with length if DNE, otherwise uses existing file length,
           passed length.
        */
        void* map(const char *filename, long &length);

        void flush(bool sync);

        void* viewOfs() {
            return view;
        }

        long length() {
            return len;
        }
        
        static void updateLength( const char *filename, long &length );
        
        static long long totalMappedLength();
        static void closeAllFiles( stringstream &message );
        
    private:
        void created();
        
        HANDLE fd;
        HANDLE maphandle;
        void *view;
        long len;
    };
    

} // namespace mongo
