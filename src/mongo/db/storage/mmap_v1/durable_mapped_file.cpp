// durable_mapped_file.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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

/**
 * this module adds some of our layers atop memory mapped files - specifically our handling of
 * private views & such if you don't care about journaling/durability (temp sort files & such) use
 * MemoryMappedFile class, not this.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"

#include <utility>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/dur_journalformat.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"

using namespace mongoutils;

namespace mongo {

using std::dec;
using std::endl;
using std::hex;
using std::map;
using std::pair;
using std::string;

void DurableMappedFile::remapThePrivateView() {
    verify(storageGlobalParams.dur);

    _willNeedRemap = false;

    // todo 1.9 : it turns out we require that we always remap to the same address.
    // so the remove / add isn't necessary and can be removed?
    void* old = _view_private;
    // privateViews.remove(_view_private);
    _view_private = remapPrivateView(_view_private);
    // privateViews.add(_view_private, this);
    fassert(16112, _view_private == old);
}

/** register view. threadsafe */
void PointerToDurableMappedFile::add_inlock(void* view, DurableMappedFile* f) {
    verify(view);
    verify(f);
    clearWritableBits_inlock(view, f->length());
    _views.insert(pair<void*, DurableMappedFile*>(view, f));
}

/** de-register view. threadsafe */
void PointerToDurableMappedFile::remove(void* view, size_t len) {
    if (view) {
        stdx::lock_guard<stdx::mutex> lk(_m);
        clearWritableBits_inlock(view, len);
        _views.erase(view);
    }
}

#ifdef _WIN32
void PointerToDurableMappedFile::clearWritableBits(void* privateView, size_t len) {
    stdx::lock_guard<stdx::mutex> lk(_m);
    clearWritableBits_inlock(privateView, len);
}

/** notification on unmapping so we can clear writable bits */
void PointerToDurableMappedFile::clearWritableBits_inlock(void* privateView, size_t len) {
    for (unsigned i = reinterpret_cast<size_t>(privateView) / MemoryMappedCOWBitset::ChunkSize;
         i <= (reinterpret_cast<size_t>(privateView) + len) / MemoryMappedCOWBitset::ChunkSize;
         ++i) {
        writable.clear(i);
        dassert(!writable.get(i));
    }
}

extern stdx::mutex mapViewMutex;

__declspec(noinline) void PointerToDurableMappedFile::makeChunkWritable(size_t chunkno) {
    stdx::lock_guard<stdx::mutex> lkPrivateViews(_m);

    if (writable.get(chunkno))  // double check lock
        return;

    // remap all maps in this chunk.
    // common case is a single map, but could have more than one with smallfiles or .ns files
    size_t chunkStart = chunkno * MemoryMappedCOWBitset::ChunkSize;
    size_t chunkNext = chunkStart + MemoryMappedCOWBitset::ChunkSize;

    stdx::lock_guard<stdx::mutex> lkMapView(mapViewMutex);

    map<void*, DurableMappedFile*>::iterator i = _views.upper_bound((void*)(chunkNext - 1));
    while (1) {
        const pair<void*, DurableMappedFile*> x = *(--i);
        DurableMappedFile* mmf = x.second;
        if (mmf == 0)
            break;

        size_t viewStart = reinterpret_cast<size_t>(x.first);
        size_t viewEnd = viewStart + mmf->length();
        if (viewEnd <= chunkStart)
            break;

        size_t protectStart = std::max(viewStart, chunkStart);
        dassert(protectStart < chunkNext);

        size_t protectEnd = std::min(viewEnd, chunkNext);
        size_t protectSize = protectEnd - protectStart;
        dassert(protectSize > 0 && protectSize <= MemoryMappedCOWBitset::ChunkSize);

        DWORD oldProtection;
        bool ok = VirtualProtect(
            reinterpret_cast<void*>(protectStart), protectSize, PAGE_WRITECOPY, &oldProtection);
        if (!ok) {
            DWORD dosError = GetLastError();

            if (dosError == ERROR_COMMITMENT_LIMIT) {
                // System has run out of memory between physical RAM & page file, tell the user
                BSONObjBuilder bb;

                ProcessInfo p;
                p.getExtraInfo(bb);

                severe() << "MongoDB has exhausted the system memory capacity.";
                severe() << "Current Memory Status: " << bb.obj().toString();
            }

            severe() << "VirtualProtect for " << mmf->filename() << " chunk " << chunkno
                     << " failed with " << errnoWithDescription(dosError) << " (chunk size is "
                     << protectSize << ", address is " << hex << protectStart << dec << ")"
                     << " in mongo::makeChunkWritable, terminating" << endl;

            fassertFailed(16362);
        }
    }

    writable.set(chunkno);
}
#else
void PointerToDurableMappedFile::clearWritableBits(void* privateView, size_t len) {}

void PointerToDurableMappedFile::clearWritableBits_inlock(void* privateView, size_t len) {}
#endif

PointerToDurableMappedFile::PointerToDurableMappedFile() {
#if defined(SIZE_MAX)
    size_t max = SIZE_MAX;
#else
    size_t max = ~((size_t)0);
#endif
    verify(max > (size_t) this);  // just checking that no one redef'd SIZE_MAX and that it is sane

    // this way we don't need any boundary checking in _find()
    _views.insert(pair<void*, DurableMappedFile*>((void*)0, (DurableMappedFile*)0));
    _views.insert(pair<void*, DurableMappedFile*>((void*)max, (DurableMappedFile*)0));
}

/** underscore version of find is for when you are already locked
    @param ofs out return our offset in the view
    @return the DurableMappedFile to which this pointer belongs
*/
DurableMappedFile* PointerToDurableMappedFile::find_inlock(void* p, /*out*/ size_t& ofs) {
    //
    // .................memory..........................
    //    v1       p                      v2
    //    [--------------------]          [-------]
    //
    // e.g., _find(p) == v1
    //
    const pair<void*, DurableMappedFile*> x = *(--_views.upper_bound(p));
    DurableMappedFile* mmf = x.second;
    if (mmf) {
        size_t o = ((char*)p) - ((char*)x.first);
        if (o < mmf->length()) {
            ofs = o;
            return mmf;
        }
    }
    return 0;
}

/** find associated MMF object for a given pointer.
    threadsafe
    @param ofs out returns offset into the view of the pointer, if found.
    @return the DurableMappedFile to which this pointer belongs. null if not found.
*/
DurableMappedFile* PointerToDurableMappedFile::find(void* p, /*out*/ size_t& ofs) {
    stdx::lock_guard<stdx::mutex> lk(_m);
    return find_inlock(p, ofs);
}

PointerToDurableMappedFile privateViews;

// here so that it is precomputed...
void DurableMappedFile::setPath(const std::string& f) {
    string suffix;
    string prefix;
    bool ok = str::rSplitOn(f, '.', prefix, suffix);
    uassert(13520,
            str::stream() << "DurableMappedFile only supports filenames in a certain format " << f,
            ok);
    if (suffix == "ns")
        _fileSuffixNo = dur::JEntry::DotNsSuffix;
    else
        _fileSuffixNo = (int)str::toUnsigned(suffix);

    _p = RelativePath::fromFullPath(storageGlobalParams.dbpath, prefix);
}

bool DurableMappedFile::open(const std::string& fname) {
    LOG(3) << "mmf open " << fname;
    invariant(!_view_write);

    setPath(fname);
    _view_write = map(fname.c_str());
    return finishOpening();
}

bool DurableMappedFile::create(const std::string& fname, unsigned long long& len) {
    LOG(3) << "mmf create " << fname;
    invariant(!_view_write);

    setPath(fname);
    _view_write = map(fname.c_str(), len);
    return finishOpening();
}

bool DurableMappedFile::finishOpening() {
    LOG(3) << "mmf finishOpening " << (void*)_view_write << ' ' << filename()
           << " len:" << length();
    if (_view_write) {
        if (storageGlobalParams.dur) {
            stdx::lock_guard<stdx::mutex> lk2(privateViews._mutex());

            _view_private = createPrivateMap();
            if (_view_private == 0) {
                msgasserted(13636,
                            str::stream() << "file " << filename() << " open/create failed "
                                                                      "in createPrivateMap "
                                                                      "(look in log for "
                                                                      "more information)");
            }
            // note that testIntent builds use this, even though it points to view_write then...
            privateViews.add_inlock(_view_private, this);
        } else {
            _view_private = _view_write;
        }
        return true;
    }
    return false;
}

DurableMappedFile::DurableMappedFile(OptionSet options)
    : MemoryMappedFile(options), _willNeedRemap(false) {
    _view_write = _view_private = 0;
}

DurableMappedFile::~DurableMappedFile() {
    try {
        LOG(3) << "mmf close " << filename();

        // If _view_private was not set, this means file open failed
        if (_view_private) {
            // Notify the durability system that we are closing a file so it can ensure we
            // will not have journaled operations with no corresponding file.
            getDur().closingFileNotification();
        }

        LockMongoFilesExclusive lk;
        privateViews.remove(_view_private, length());

        MemoryMappedFile::close();
    } catch (...) {
        error() << "exception in ~DurableMappedFile";
    }
}
}
