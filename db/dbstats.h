// dbstats.h

#include "../stdafx.h"
#include "jsobj.h"
#include "../util/message.h"

namespace mongo {

    /**
     * for storing operation counters
     * note: not thread safe.  ok with that for speed
     */
    class OpCounters {
    public:
        
        OpCounters();

        int * getInsert(){ return _insert; }
        int * getQuery(){ return _query; }
        int * getUpdate(){ return _update; }
        int * getDelete(){ return _delete; }
        int * getGetGore(){ return _getmore; }

        void gotInsert(){ _insert[0]++; }
        void gotQuery(){ _query[0]++; }
        void gotUpdate(){ _update[0]++; }
        void gotDelete(){ _delete[0]++; }
        void gotGetMore(){ _getmore[0]++; }

        void gotOp( int op );

        BSONObj& getObj(){ return _obj; }
    private:
        BSONObj _obj;
        int * _insert;
        int * _query;
        int * _update;
        int * _delete;
        int * _getmore;
    };

    extern OpCounters globalOpCounters;

}
