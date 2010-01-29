// dbstats.cpp

#include "stdafx.h"
#include "dbstats.h"

namespace mongo {

    OpCounters::OpCounters(){
        int zero = 0;

        BSONObjBuilder b;
        b.append( "insert" , zero );
        b.append( "query" , zero );
        b.append( "update" , zero );
        b.append( "delete" , zero );
        b.append( "getmore" , zero );
        _obj = b.obj();

        _insert = (int*)_obj["insert"].value();
        _query = (int*)_obj["query"].value();
        _update = (int*)_obj["update"].value();
        _delete = (int*)_obj["delete"].value();
        _getmore = (int*)_obj["getmore"].value();
    }

    void OpCounters::gotOp( int op ){
        switch ( op ){
        case dbInsert: gotInsert(); break;
        case dbQuery: gotQuery(); break;
        case dbUpdate: gotUpdate(); break;
        case dbDelete: gotDelete(); break;
        case dbGetMore: gotGetMore(); break;
        case dbKillCursors:
        case opReply:
        case dbMsg:
            break;
        default: log() << "OpCounters::gotOp unknown op: " << op << endl;
        }
    }
    
    IndexCounters::IndexCounters(){
        _memSupported = _pi.blockCheckSupported();
        
        _btreeMemHits = 0;
        _btreeMemMisses = 0;
        _btreeAccesses = 0;
        
        
        _maxAllowed = ( numeric_limits< long long >::max() ) / 2;
        _resets = 0;

        _sampling = 0;
        _samplingrate = 100;
    }
    
    void IndexCounters::append( BSONObjBuilder& b ){
        if ( ! _memSupported ){
            b.append( "note" , "not supported on this platform" );
            return;
        }

        BSONObjBuilder bb( b.subobjStart( "btree" ) );
        if ( _btreeAccesses < ( numeric_limits<int>::max() / 2 ) ){
            bb.append( "accesses" , (int)_btreeAccesses );
            bb.append( "hits" , (int)_btreeMemHits);
            bb.append( "misses" , (int)_btreeMemMisses );
            
        }
        else {
            bb.append( "accesses" , _btreeAccesses );
            bb.append( "hits" , _btreeMemHits );
            bb.append( "misses" , _btreeMemMisses );
        }
        bb.append( "resets" , _resets );
        
        bb.append( "missRatio" , (double)_btreeMemMisses / (double)_btreeAccesses );
        
        bb.done();
        
        if ( _btreeAccesses > _maxAllowed ){
            _btreeAccesses = 0;
            _btreeMemMisses = 0;
            _btreeMemHits = 0;
            _resets++;
        }
    }
    

    OpCounters globalOpCounters;
    IndexCounters globalIndexCounters;
}
