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
//#include "../util/moveablebuffer.h"

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
           @return the private view.
                   on _DEBUG, returns the readonly view
        */
        void* getView();

        /* switch to _view_write.  normally, this is a bad idea since your changes will not 
           show up in _view_private if there have been changes there; thus the leading underscore
           as a tad of a "warning".  but useful when done with some care, such as during 
           initialization.
        */
        static void* _switchToWritableView(void *private_ptr);

        /** for _TESTINTENT build.
            translates the read view pointer into a pointer to the corresponding 
            place in the private view.
        */
        static void* switchToPrivateView(void *debug_readonly_ptr);
        
        /** for a filename a/b/c.3
            filePath() is "a/b/c"
            fileSuffixNo() is 3
            if the suffix is "ns", fileSuffixNo -1
        */
        string relativePath() const { return _relativePath; }
        int fileSuffixNo() const { return _fileSuffixNo; }
        void* view_write() { return _view_write; }

        /** true if we have written.  
            set in PREPLOGBUFFER, it is NOT set immediately on write intent declaration.
            reset to false in REMAPPRIVATEVIEW
        */
        bool& willNeedRemap() { return _willNeedRemap; }

        void remapThePrivateView();

        virtual bool isMongoMMF() { return true; }

    private:

        void *_view_write;
        void *_view_private;
        void *_view_readonly; // for _DEBUG build
        bool _willNeedRemap;
        string _relativePath;   // e.g. "somepath/dbname"
        int _fileSuffixNo;  // e.g. 3.  -1="ns"

        void setPath(string pathAndFileName);
        bool finishOpening();
    };

    /** for durability support we want to be able to map pointers to specific MongoMMF objects. 
    */
    class PointerToMMF : boost::noncopyable { 
    public:
        PointerToMMF();

        /** register view. threadsafe */
        void add(void *view, MongoMMF *f);

        /** de-register view. threadsafe */
        void remove(void *view);

        /** find associated MMF object for a given pointer.
            threadsafe
            @param ofs out returns offset into the view of the pointer, if found.
            @return the MongoMMF to which this pointer belongs. null if not found.
        */
        MongoMMF* find(void *p, /*out*/ size_t& ofs);

        /** for doing many finds in a row with one lock operation */
        mutex& _mutex() { return _m; }
        MongoMMF* _find(void *p, /*out*/ size_t& ofs);

    private:
        mutex _m;
        map<void*, MongoMMF*> _views;
    };

    // allows a pointer into any private view of a MongoMMF to be resolved to the MongoMMF object
    extern PointerToMMF privateViews;
}
