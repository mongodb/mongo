// @file dur_stats.h

namespace mongo {
    namespace dur {

        /** journalling stats.  the model here is that the commit thread is the only writer, and that reads are 
            uncommon (from a serverStatus command and such).  Thus, there should not be multicore chatter overhead.
        */
        struct Stats { 
            Stats();
            struct S { 
                unsigned _commits;
                unsigned _objCopies;
                unsigned long long _journaledBytes;
                unsigned long long _writeToDataFilesBytes;
            } curr;
        };
        extern Stats stats;

    }
}
