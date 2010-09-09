/** @file capped.h capped collections
  */

#pragma once

#include "cursor.h"

namespace mongo {

    class NamespaceDetails;

    namespace cappedcollection { 
        // must be in write lock for these:
        void create(string path, string ns, NamespaceDetails *nsd, unsigned long long approxSize);
        void open  (string path, string db, NamespaceDetails *nsd);

        class Insert : boost::noncopyable {
        public:
            void* start(unsigned recLen);
            DiskLoc finish();
        //private:
        //    CappedCollection *_cc;
        };

    }

    /* operations 
       closefile???
       open
       create
       insert
       insert getting a ptr and then finishing
       truncateFrom
       drop
       empty
       cursors
       */

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
