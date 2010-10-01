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

    /* Adds some of our layers atop memory mapped files - specifically our handling of private views & such 
       if you don't care about journaling/durability (temp sort files & such) use MemoryMappedFile class, not this.
    */
    class MongoMMF : private MemoryMappedFile { 
    public:
        MongoMMF();
        ~MongoMMF();
        unsigned long long length() const { return MemoryMappedFile::length(); }
        bool open(string fname);
        bool create(string fname, unsigned long long& len);

        // we will re-map the private few frequently, thus the use of MoveableBuffer
        MoveableBuffer getView();

        // for _DEBUG build
        static void* switchToWritableView(void *);

    private:
        void *view_write;
        void *view_private;
        void *view_readonly; // for _DEBUG build
    };

}
