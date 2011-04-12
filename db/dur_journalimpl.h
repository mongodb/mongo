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

            /** write to journal
            */
            void journal(const AlignedBuilder& b);

            boost::filesystem::path getFilePathFor(int filenumber) const;

            unsigned long long lastFlushTime() const { return _lastFlushTime; }
            void cleanup(bool log);

            // Rotate after reaching this data size in a journal (j._<n>) file
            // We use a smaller size for 32 bit as the journal is mmapped during recovery (only)
            // Note if you take a set of datafiles, including journal files, from 32->64 or vice-versa, it must 
            // work.  (and should as-is)
#if defined(_DEBUG)
            static const unsigned long long DataLimit = 128 * 1024 * 1024;
#else
            static const unsigned long long DataLimit = (sizeof(void*)==4) ? 256 * 1024 * 1024 : 1 * 1024 * 1024 * 1024;
#endif

            unsigned long long curFileId() const { return _curFileId; }

            void assureLogFileOpen() {
                mutex::scoped_lock lk(_curLogFileMutex);
                if( _curLogFile == 0 )
                    _open();
            }

            /** open a journal file to journal operations to. */
            void open();

        private:
            void _open();
            void closeCurrentJournalFile();
            void removeUnneededJournalFiles();

            unsigned long long _written; // bytes written so far to the current journal (log) file
            unsigned _nextFileNumber;

            mutex _curLogFileMutex;

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
