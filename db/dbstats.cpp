// dbstats.cpp

#include "../stdafx.h"
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
        default: log() << "OpCounters::gotOp unkonwn op: " << op << endl;
        }
    }


    OpCounters globalOpCounters;
}
