// @file dur_stats.h

namespace mongo {
    namespace dur {

        /** journalling stats.  the model here is that the commit thread is the only writer, and that reads are 
            uncommon (from a serverStatus command and such).  Thus, there should not be multicore chatter overhead.
        */
        struct Stats { 
            Stats() { curr.reset(); }
            struct S { 
                BSONObj asObj();
                void reset();

                unsigned _commits;
                unsigned long long _journaledBytes;
                unsigned long long _writeToDataFilesBytes;

                // undesirable to be in write lock for the group commit (it can be done in a read lock), so good if we 
                // have visibility when this happens.  can happen for a couple reasons
                // - read lock starvation
                // - file being closed
                // - data being written faster than the normal group commit interval
                unsigned _commitsInWriteLock; 
            } curr;
        };
        extern Stats stats;

    }
}
