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

        /** journaling stats.  the model here is that the commit thread is the only writer, and that reads are
            uncommon (from a serverStatus command and such).  Thus, there should not be multicore chatter overhead.
        */
        struct Stats {
            Stats();
            void rotate();
            BSONObj asObj();
            unsigned _intervalMicros;

            struct S {
                BSONObj _asObj();
                std::string _asCSV();
                std::string _CSVHeader();
                void reset();

                unsigned _commits;
                unsigned long long _journaledBytes;
                unsigned long long _uncompressedBytes;
                unsigned long long _writeToDataFilesBytes;

                long long _prepLogBufferMicros;
                long long _writeToJournalMicros;
                long long _writeToDataFilesMicros;
                long long _remapPrivateViewMicros;

                int _dtMillis;
            };

            S *curr;
        private:
            S _a,_b;
            unsigned long long _lastRotate;
            S* other();
        };
        extern Stats stats;

    }
}
