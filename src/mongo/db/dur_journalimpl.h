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

#include "dur_journalformat.h"
#include "../util/logfile.h"

namespace mongo {
    namespace dur {

        /** the writeahead journal for durability */
        class Journal {
        public:
            string dir; // set by journalMakeDir() during initialization

            Journal();

            /** call during startup by journalMakeDir() */
            void init();

            /** check if time to rotate files.  assure a file is open.
                done separately from the journal() call as we can do this part
                outside of lock.
                thread: durThread()
             */
            void rotate();

            /** append to the journal file
            */
            void journal(const JSectHeader& h, const AlignedBuilder& b);

            boost::filesystem::path getFilePathFor(int filenumber) const;

            unsigned long long lastFlushTime() const { return _lastFlushTime; }
            void cleanup(bool log); // closes and removes journal files

            unsigned long long curFileId() const { return _curFileId; }

            void assureLogFileOpen() {
                SimpleMutex::scoped_lock lk(_curLogFileMutex);
                if( _curLogFile == 0 )
                    _open();
            }

            /** open a journal file to journal operations to. */
            void open();

        private:
            /** check if time to rotate files.  assure a file is open.
             *  internally called with every commit
             */
            void _rotate();

            void _open();
            void closeCurrentJournalFile();
            void removeUnneededJournalFiles();

            unsigned long long _written; // bytes written so far to the current journal (log) file
            unsigned _nextFileNumber;
        public:
            SimpleMutex _curLogFileMutex;
            bool _ageOut;
        private:

            LogFile *_curLogFile; // use _curLogFileMutex
            unsigned long long _curFileId; // current file id see JHeader::fileId

            struct JFile {
                string filename;
                unsigned long long lastEventTimeMs;
            };

            // files which have been closed but not unlinked (rotated out) yet
            // ordered oldest to newest
            list<JFile> _oldJournalFiles; // use _curLogFileMutex

            // lsn related
            static void preFlush();
            static void postFlush();
            unsigned long long _preFlushTime;
            unsigned long long _lastFlushTime; // data < this time is fsynced in the datafiles (unless hard drive controller is caching)
            bool _writeToLSNNeeded;
            void updateLSNFile();
        };

    }
}
