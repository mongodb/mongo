/** @file mongommf.h
*
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

#include "../util/mmap.h"
#include "../util/moveablebuffer.h"

namespace mongo {

    /** MongoMMF adds some layers atop memory mapped files - specifically our handling of private views & such.
        if you don't care about journaling/durability (temp sort files & such) use MemoryMappedFile class, 
        not this.
    */
    class MongoMMF : private MemoryMappedFile { 
    public:
        MongoMMF();
        virtual ~MongoMMF();
        virtual void close();
        bool open(string fname, bool sequentialHint);
        unsigned long long length() const { return MemoryMappedFile::length(); }
        string filename() const { return MemoryMappedFile::filename(); }
        void flush(bool sync)   { MemoryMappedFile::flush(sync); }

        /* Creates with length if DNE, otherwise uses existing file length,
           passed length.
           @param sequentialHint if true will be sequentially accessed
           @return true for ok
        */
        bool create(string fname, unsigned long long& len, bool sequentialHint);

        /* Get the "standard" view (which is the private one).
           We re-map the private view frequently, thus the use of MoveableBuffer 
           use.
           @return the private view.
                   on _DEBUG, returns the readonly view
        */
        MoveableBuffer getView();

        /* switch to _view_write.  normally, this is a bad idea since your changes will not 
           show up in _view_private if there have been changes there; thus the leading underscore
           as a tad of a "warning".  but useful when done with some care, such as during 
           initialization.
        */
        static void* _switchToWritableView(void *private_ptr);

        /** for _DEBUG build.
            translates the read view pointer into a pointer to the corresponding 
            place in the private view.
        */
        static void* switchToPrivateView(void *debug_readonly_ptr);

    private:
        void *_view_write;
        void *_view_private;
        void *_view_readonly; // for _DEBUG build
    };

}
