// counters.cpp
/*
 *    Copyright (C) 2010 10gen Inc.
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


#include "stdafx.h"
#include "../jsobj.h"
#include "counters.h"

namespace mongo {

    OpCounters::OpCounters(){
        int zero = 0;

        BSONObjBuilder b;
        b.append( "insert" , zero );
        b.append( "query" , zero );
        b.append( "update" , zero );
        b.append( "delete" , zero );
        b.append( "getmore" , zero );
        b.append( "command" , zero );
        _obj = b.obj();

        _insert = (int*)_obj["insert"].value();
        _query = (int*)_obj["query"].value();
        _update = (int*)_obj["update"].value();
        _delete = (int*)_obj["delete"].value();
        _getmore = (int*)_obj["getmore"].value();
        _command = (int*)_obj["command"].value();
    }

    void OpCounters::gotOp( int op , bool isCommand ){
        switch ( op ){
        case dbInsert: gotInsert(); break;
        case dbQuery: 
            if ( isCommand )
                gotCommand();
            else 
                gotQuery(); 
            break;
            
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
        bb.appendIntOrLL( "accesses" , _btreeAccesses );
        bb.appendIntOrLL( "hits" , _btreeMemHits );
        bb.appendIntOrLL( "misses" , _btreeMemMisses );

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
