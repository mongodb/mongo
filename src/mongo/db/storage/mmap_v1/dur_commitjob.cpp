/* @file dur_commitjob.cpp */

/**
*    Copyright (C) 2009 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/dur_commitjob.h"

#include <iostream>

#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

using std::shared_ptr;
using std::endl;
using std::max;
using std::min;

namespace dur {

void WriteIntent::absorb(const WriteIntent& other) {
    dassert(overlaps(other));

    void* newStart = min(start(), other.start());
    p = max(p, other.p);
    len = (char*)p - (char*)newStart;

    dassert(contains(other));
}


CommitJob::CommitJob() : _hasWritten(false), _lastNotedPos(0), _bytes(0) {}

CommitJob::~CommitJob() {}

void CommitJob::noteOp(shared_ptr<DurOp> p) {
    stdx::lock_guard<SimpleMutex> lk(groupCommitMutex);
    _hasWritten = true;
    _durOps.push_back(p);
}

void CommitJob::note(void* p, int len) {
    _hasWritten = true;

    if (!_alreadyNoted.checkAndSet(p, len)) {
        // Remember intent. We will journal it in a bit.
        _insertWriteIntent(p, len);

        // Round off to page address (4KB).
        const size_t x = ((size_t)p) & ~0xfff;

        if (x != _lastNotedPos) {
            _lastNotedPos = x;

            // Add the full page amount
            _bytes += (len + 4095) & ~0xfff;

            if (_bytes > UncommittedBytesLimit * 3) {
                _complains++;

                // Throttle logging
                if (_complains < 100 || (curTimeMillis64() - _lastComplainMs >= 60000)) {
                    _lastComplainMs = curTimeMillis64();

                    warning() << "DR102 too much data written uncommitted (" << _bytes / 1000000.0
                              << "MB)";

                    if (_complains < 10 || _complains % 10 == 0) {
                        printStackTrace();
                    }
                }
            }
        }
    }
}

void CommitJob::committingReset() {
    _hasWritten = false;
    _alreadyNoted.clear();
    _intents.clear();
    _durOps.clear();
    _bytes = 0;
}

}  // namespace "dur"
}  // namespace "mongo"
