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
            MVar<unsigned long long> &toStoreLastSeqNum;

            Journal();

            void init();

            void rotate();

            void journal(const AlignedBuilder& b);

            boost::filesystem::path getFilePathFor(int filenumber) const;

            /** used at shutdown.
                @return false if can't close in a timely manner. 
            */
            bool tryToCloseCurJournalFile();

        private:
            static const unsigned long long DataLimit = 1 * 1024 * 1024 * 1024;

            /** open a journal file to journal operations to. */
            void open();

            void _open();
            void closeCurrentJournalFile();
            void removeUnneededJournalFiles();

            unsigned long long _written; // bytes written so far to the current journal (log) file
            unsigned _nextFileNumber;

            mutex _curLogFileMutex;

            LogFile *_curLogFile; // use _curLogFileMutex

            struct JFile { 
                string filename;
                unsigned long long lastEventTimeMs;
            };

            // files which have been closed but not unlinked (rotated out) yet
            // ordered oldest to newest
            list<JFile> _oldJournalFiles; // use _curLogFileMutex

            static void preFlush();
            static void postFlush();
            unsigned long long _preFlushTime;
            unsigned long long _lastFlushTime; // data < this time is fsynced in the datafiles (unless hard drive controller is caching)
        };

    }
}
