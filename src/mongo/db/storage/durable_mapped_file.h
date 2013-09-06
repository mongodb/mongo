// durable_mapped_file.h

/*
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

#pragma once

#include "mongo/util/mmap.h"
#include "mongo/util/paths.h"

namespace mongo {

    /** DurableMappedFile adds some layers atop memory mapped files - specifically our handling of private views & such.
        if you don't care about journaling/durability (temp sort files & such) use MemoryMappedFile class,
        not this.
    */
    class DurableMappedFile : private MemoryMappedFile {
    protected:
        virtual void* viewForFlushing() { return _view_write; }

    public:
        DurableMappedFile();
        virtual ~DurableMappedFile();
        virtual void close();

        /** @return true if opened ok. */
        bool open(const std::string& fname, bool sequentialHint /*typically we open with this false*/);

        /** @return file length */
        unsigned long long length() const { return MemoryMappedFile::length(); }

        string filename() const { return MemoryMappedFile::filename(); }

        void flush(bool sync)   { MemoryMappedFile::flush(sync); }

        /* Creates with length if DNE, otherwise uses existing file length,
           passed length.
           @param sequentialHint if true will be sequentially accessed
           @return true for ok
        */
        bool create(const std::string& fname, unsigned long long& len, bool sequentialHint);

        /* Get the "standard" view (which is the private one).
           @return the private view.
        */
        void* getView() const { return _view_private; }
        
        /* Get the "write" view (which is required for writing).
           @return the write view.
        */
        void* view_write() const { return _view_write; }

        /** for a filename a/b/c.3
            filePath() is "a/b/c"
            fileSuffixNo() is 3
            if the suffix is "ns", fileSuffixNo -1
        */
        const RelativePath& relativePath() const {
            DEV verify( !_p._p.empty() );
            return _p;
        }

        int fileSuffixNo() const { return _fileSuffixNo; }
        HANDLE getFd() { return MemoryMappedFile::getFd(); }

        /** true if we have written.
            set in PREPLOGBUFFER, it is NOT set immediately on write intent declaration.
            reset to false in REMAPPRIVATEVIEW
        */
        bool& willNeedRemap() { return _willNeedRemap; }

        void remapThePrivateView();

        virtual bool isDurableMappedFile() { return true; }

    private:

        void *_view_write;
        void *_view_private;
        bool _willNeedRemap;
        RelativePath _p;   // e.g. "somepath/dbname"
        int _fileSuffixNo;  // e.g. 3.  -1="ns"

        void setPath(const std::string& pathAndFileName);
        bool finishOpening();
    };

    /** for durability support we want to be able to map pointers to specific DurableMappedFile objects.
    */
    class PointerToDurableMappedFile : boost::noncopyable {
    public:
        PointerToDurableMappedFile();

        /** register view.
            threadsafe
            */
        void add(void *view, DurableMappedFile *f);

        /** de-register view.
            threadsafe
            */
        void remove(void *view);

        /** find associated MMF object for a given pointer.
            threadsafe
            @param ofs out returns offset into the view of the pointer, if found.
            @return the DurableMappedFile to which this pointer belongs. null if not found.
        */
        DurableMappedFile* find(void *p, /*out*/ size_t& ofs);

        /** for doing many finds in a row with one lock operation */
        mutex& _mutex() { return _m; }
        DurableMappedFile* find_inlock(void *p, /*out*/ size_t& ofs);

        map<void*,DurableMappedFile*>::iterator finditer_inlock(void *p) { return _views.upper_bound(p); }

        unsigned numberOfViews_inlock() const { return _views.size(); }

    private:
        mutex _m;
        map<void*, DurableMappedFile*> _views;
    };

    // allows a pointer into any private view of a DurableMappedFile to be resolved to the DurableMappedFile object
    extern PointerToDurableMappedFile privateViews;
}
