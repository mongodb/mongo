// @file dur.h durability support

#pragma once
namespace mongo {
    class MemoryMappedFile;

    namespace dur {
        class ParsedJournalEntry;

       /** call go() to execute a recovery from existing journal files.
        */
        class RecoveryJob : boost::noncopyable { 
        public:
            RecoveryJob() { _lastDataSyncedFromLastRun = 0; }
            void go(vector<path>& files);
            ~RecoveryJob();
            void processSection(const void *, unsigned len);
            void close();
        private:
            void applyEntry(const ParsedJournalEntry& entry, bool apply, bool dump);
            void applyEntries(const vector<ParsedJournalEntry> &entries);
            bool processFileBuffer(const void *, unsigned len);
            bool processFile(path journalfile);

            /** retrieve the mmap pointer for the specified dbName plus file number.
                open if not yet open.
                @param fileNo a value of -1 indicates ".ns"
                @param ofs offset to add to the pointer before returning
            */
            void* ptr(const char *dbName, int fileNo, unsigned ofs);

            // fileno,dbname -> map
            map< pair<int,string>, void* > _fileToPtr;

            // all close at end (destruction) of RecoveryJob
            list< shared_ptr<MemoryMappedFile> > _files;

            unsigned long long _lastDataSyncedFromLastRun;
        };
    }
}
