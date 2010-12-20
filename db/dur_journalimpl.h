// @file dur_journal.h

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
*/

#pragma once

#include "../util/concurrency/mvar.h"

namespace mongo {
    namespace dur {

        class Journal {
        public:
            string dir; // set by journalMakeDir() during initialization
            MVar<path> &toUnlink; // unlinks of old journal threads are via background thread

            Journal();

            void open();
            void rotate();
            void journal(const CompressedBuilder& b);

            path getFilePathFor(int filenumber) const;

            /** used at shutdown.
                @return false if can't close in a timely manner. 
            */
            bool tryToCloseCurLogFile();

        private:
            static const unsigned long long DataLimit = 1 * 1024 * 1024 * 1024;

            void _open();

            unsigned long long _written; // bytes written so far to the current journal (log) file
            unsigned _nextFileNumber;
            LogFile *_curLogFile;
            mutex _curLogFileMutex; // lock when using _curLogFile
        };

    }
}
