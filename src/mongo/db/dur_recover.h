// @file dur.h durability support

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
*/

#pragma once

#include <boost/filesystem/operations.hpp>
#include <list>

#include "mongo/db/dur_journalformat.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/file.h"

namespace mongo {
    class MongoMMF;

    namespace dur {
        struct ParsedJournalEntry;

        /** call go() to execute a recovery from existing journal files.
         */
        class RecoveryJob : boost::noncopyable {
            static class Last {
            public:
                Last();
                MongoMMF* newEntry(const ParsedJournalEntry&, RecoveryJob&);
            private:
                MongoMMF *mmf;
                string dbName;
                int fileNo;
            } last;        
        public:
            RecoveryJob() : _lastDataSyncedFromLastRun(0), 
                _mx("recovery"), _recovering(false) { _lastSeqMentionedInConsoleLog = 1; }
            void go(vector<boost::filesystem::path>& files);
            ~RecoveryJob();

            /** @param data data between header and footer. compressed if recovering. */
            void processSection(const JSectHeader *h, const void *data, unsigned len, const JSectFooter *f);

            void close(); // locks and calls _close()

            static RecoveryJob & get() { return _instance; }
        private:
            void write(Last& last, const ParsedJournalEntry& entry); // actually writes to the file
            void applyEntry(Last& last, const ParsedJournalEntry& entry, bool apply, bool dump);
            void applyEntries(const vector<ParsedJournalEntry> &entries);
            bool processFileBuffer(const void *, unsigned len);
            bool processFile(boost::filesystem::path journalfile);
            void _close(); // doesn't lock
            MongoMMF* getMongoMMF(const ParsedJournalEntry& entry);

            list<boost::shared_ptr<MongoMMF> > _mmfs;

            unsigned long long _lastDataSyncedFromLastRun;
            unsigned long long _lastSeqMentionedInConsoleLog;
        public:
            mongo::mutex _mx; // protects _mmfs
        private:
            bool _recovering; // are we in recovery or WRITETODATAFILES

            static RecoveryJob &_instance;
        };
    }
}
