// top.cpp
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


#include "pch.h"
#include "top.h"
#include "../../util/message.h"
#include "../commands.h"

namespace mongo {
    
    Top::UsageData::UsageData( const UsageData& older , const UsageData& newer )
        : time(newer.time-older.time) , 
          count(newer.count-older.count) 
    {
        
    }

    Top::CollectionData::CollectionData( const CollectionData& older , const CollectionData& newer )
        : total( older.total , newer.total ) , 
          readLock( older.readLock , newer.readLock ) ,
          writeLock( older.writeLock , newer.writeLock ) ,
          queries( older.queries , newer.queries ) ,
          getmore( older.getmore , newer.getmore ) ,
          insert( older.insert , newer.insert ) ,
          update( older.update , newer.update ) ,
          remove( older.remove , newer.remove ),
          commands( older.commands , newer.commands ) 
    {
        
    }

    
    void Top::record( const string& ns , int op , int lockType , long long micros , bool command ){
        //cout << "record: " << ns << "\t" << op << "\t" << command << endl;
        scoped_lock lk(_lock);
        
        if ( ( command || op == dbQuery ) && ns == _lastDropped ){
            _lastDropped = "";
            return;
        }

        CollectionData& coll = _usage[ns];
        _record( coll , op , lockType , micros , command );
        _record( _global , op , lockType , micros , command );
    }

    void Top::collectionDropped( const string& ns ){
        //cout << "collectionDropped: " << ns << endl;
        scoped_lock lk(_lock);
        _usage.erase(ns);
        _lastDropped = ns;
    }
    
    void Top::_record( CollectionData& c , int op , int lockType , long long micros , bool command ){
        c.total.inc( micros );
        
        if ( lockType > 0 )
            c.writeLock.inc( micros );
        else if ( lockType < 0 )
            c.readLock.inc( micros );
        
        switch ( op ){
        case 0:
            // use 0 for unknown, non-specific
            break;
        case dbUpdate:
            c.update.inc( micros );
            break;
        case dbInsert:
            c.insert.inc( micros );
            break;
        case dbQuery:
            if ( command )
                c.commands.inc( micros );
            else
                c.queries.inc( micros );
            break;
        case dbGetMore:
            c.getmore.inc( micros );
            break;
        case dbDelete:
            c.remove.inc( micros );
            break;
        case dbKillCursors:
            break;
        case opReply: 
        case dbMsg:
            log() << "unexpected op in Top::record: " << op << endl;
            break;
        default:
            log() << "unknown op in Top::record: " << op << endl;
        }

    }

    void Top::cloneMap(Top::UsageMap& out){
        scoped_lock lk(_lock);
        out = _usage;
    }

    void Top::append( BSONObjBuilder& b ){
        scoped_lock lk( _lock );
        append( b , _usage );
    }

    void Top::append( BSONObjBuilder& b , const char * name , const UsageData& map ){
        BSONObjBuilder bb( b.subobjStart( name ) );
        bb.appendNumber( "time" , map.time );
        bb.appendNumber( "count" , map.count );
        bb.done();
    }

    void Top::append( BSONObjBuilder& b , const UsageMap& map ){
        for ( UsageMap::const_iterator i=map.begin(); i!=map.end(); i++ ){
            BSONObjBuilder bb( b.subobjStart( i->first ) );
            
            const CollectionData& coll = i->second;
            
            append( b , "total" , coll.total );
            
            append( b , "readLock" , coll.readLock );
            append( b , "writeLock" , coll.writeLock );

            append( b , "queries" , coll.queries );
            append( b , "getmore" , coll.getmore );
            append( b , "insert" , coll.insert );
            append( b , "update" , coll.update );
            append( b , "remove" , coll.remove );
            append( b , "commands" , coll.commands );
            
            bb.done();
        }
    }

    class TopCmd : public Command {
    public:
        TopCmd() : Command( "top", true ){}

        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return READ; } 
        virtual void help( stringstream& help ) const { help << "usage by collection"; }

        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl){
            {
                BSONObjBuilder b( result.subobjStart( "totals" ) );
                Top::global.append( b );
                b.done();
            }
            return true;
        }
        
    } topCmd;

    Top Top::global;
    
    TopOld::T TopOld::_snapshotStart = TopOld::currentTime();
    TopOld::D TopOld::_snapshotDuration;
    TopOld::UsageMap TopOld::_totalUsage;
    TopOld::UsageMap TopOld::_snapshotA;
    TopOld::UsageMap TopOld::_snapshotB;
    TopOld::UsageMap &TopOld::_snapshot = TopOld::_snapshotA;
    TopOld::UsageMap &TopOld::_nextSnapshot = TopOld::_snapshotB;
    mongo::mutex TopOld::topMutex("topMutex");


}
