// @file dur_stats.h

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/jsobj.h"

namespace mongo {
namespace dur {

/**
 * journaling stats.  the model here is that the commit thread is the only writer, and that reads
 * are uncommon (from a serverStatus command and such).  Thus, there should not be multicore chatter
 * overhead.
 */
struct Stats {
    struct S {
        std::string _CSVHeader() const;
        std::string _asCSV() const;

        void _asObj(BSONObjBuilder* builder) const;

        void reset();

        uint64_t getCurrentDurationMillis() const {
            return ((curTimeMicros64() - _startTimeMicros) / 1000);
        }


        // Not reported. Internal use only.
        uint64_t _startTimeMicros;

        // Reported statistics
        unsigned _durationMillis;

        unsigned _commits;
        unsigned _commitsInWriteLock;

        uint64_t _journaledBytes;
        uint64_t _uncompressedBytes;
        uint64_t _writeToDataFilesBytes;

        uint64_t _prepLogBufferMicros;
        uint64_t _writeToJournalMicros;
        uint64_t _writeToDataFilesMicros;
        uint64_t _remapPrivateViewMicros;
        uint64_t _commitsMicros;
        uint64_t _commitsInWriteLockMicros;
    };


    Stats();
    void reset();

    BSONObj asObj() const;

    const S* curr() const {
        return &_stats[_currIdx];
    }
    S* curr() {
        return &_stats[_currIdx];
    }

private:
    S _stats[5];
    unsigned _currIdx;
};

extern Stats stats;
}
}
