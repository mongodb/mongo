// @file dur.h durability support

#pragma once

#include "dur_journalformat.h"
#include "../util/concurrency/mutex.h"
#include "../util/file.h"

namespace mongo {
    class MongoMMF;

    namespace dur {
        struct ParsedJournalEntry;

        /** call go() to execute a recovery from existing journal files.
         */
        class RecoveryJob : boost::noncopyable {
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
            void write(const ParsedJournalEntry& entry, MongoMMF* mmf); // actually writes to the file
            void applyEntry(const ParsedJournalEntry& entry, bool apply, bool dump, MongoMMF* mmf);
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
