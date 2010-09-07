/** @file capped.h capped collections
  */

#pragma once

#include "../cursor.h"

namespace mongo {

    class CappedCollection : boost::noncopyable { 
    public:
        /* open */
        CappedCollection(string z, const string &dbpath, const string& ns);

        /* create */
        CappedCollection(const string& dbpath, const string& ns, unsigned long long size);

        /* closes file */
        ~CappedCollection();

        struct InsertJob : boost::noncopyable { 
            void * data;
            DiskLoc loc;
            InsertJob(CappedCollection *cc);
            ~InsertJob();
        private:
            CappedCollection *_cc;
        };

        void truncateFrom(DiskLoc x);

        // drop?
    };

    class ForwardCappedCursor2 : public BasicCursor, public AdvanceStrategy {
    public:
        ForwardCappedCursor2( NamespaceDetails *nsd = 0, const DiskLoc &startLoc = DiskLoc() );
        virtual string toString() { return "ForwardCappedCursor2"; }
        virtual DiskLoc next( const DiskLoc &prev ) const;
        virtual bool capped() const { return true; }
    private:
        NamespaceDetails *nsd;
    };

    class ReverseCappedCursor2 : public BasicCursor, public AdvanceStrategy {
    public:
        ReverseCappedCursor2( NamespaceDetails *nsd = 0, const DiskLoc &startLoc = DiskLoc() );
        virtual string toString() { return "ReverseCappedCursor2"; }
        virtual DiskLoc next( const DiskLoc &prev ) const;
        virtual bool capped() const { return true; }
    private:
        NamespaceDetails *nsd;
    };

}
