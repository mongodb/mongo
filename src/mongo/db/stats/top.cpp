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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/pch.h"

#include "mongo/db/stats/top.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/util/net/message.h"
#include "mongo/db/commands.h"

namespace mongo {

    Top::UsageData::UsageData( const UsageData& older , const UsageData& newer ) {
        // this won't be 100% accurate on rollovers and drop(), but at least it won't be negative
        time  = (newer.time  >= older.time)  ? (newer.time  - older.time)  : newer.time;
        count = (newer.count >= older.count) ? (newer.count - older.count) : newer.count;
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
          commands( older.commands , newer.commands ) {

    }

    void Top::record( const StringData& ns , int op , int lockType , long long micros , bool command ) {
        if ( ns[0] == '?' )
            return;

        //cout << "record: " << ns << "\t" << op << "\t" << command << endl;
        SimpleMutex::scoped_lock lk(_lock);

        if ( ( command || op == dbQuery ) && ns == _lastDropped ) {
            _lastDropped = "";
            return;
        }

        CollectionData& coll = _usage[ns];
        _record( coll , op , lockType , micros , command );
        _record( _global , op , lockType , micros , command );
    }

    void Top::_record( CollectionData& c , int op , int lockType , long long micros , bool command ) {
        c.total.inc( micros );

        if ( lockType > 0 )
            c.writeLock.inc( micros );
        else if ( lockType < 0 )
            c.readLock.inc( micros );

        switch ( op ) {
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

    void Top::collectionDropped( const string& ns ) {
        //cout << "collectionDropped: " << ns << endl;
        SimpleMutex::scoped_lock lk(_lock);
        _usage.erase(ns);
        _lastDropped = ns;
    }

    void Top::cloneMap(Top::UsageMap& out) const {
        SimpleMutex::scoped_lock lk(_lock);
        out = _usage;
    }

    void Top::append( BSONObjBuilder& b ) {
        SimpleMutex::scoped_lock lk( _lock );
        _appendToUsageMap( b , _usage );
    }

    void Top::_appendToUsageMap( BSONObjBuilder& b , const UsageMap& map ) const {
        // pull all the names into a vector so we can sort them for the user
        
        vector<string> names;
        for ( UsageMap::const_iterator i = map.begin(); i != map.end(); ++i ) {
            names.push_back( i->first );
        }
        
        std::sort( names.begin(), names.end() );

        for ( size_t i=0; i<names.size(); i++ ) {
            BSONObjBuilder bb( b.subobjStart( names[i] ) );

            const CollectionData& coll = map.find(names[i])->second;

            _appendStatsEntry( b , "total" , coll.total );

            _appendStatsEntry( b , "readLock" , coll.readLock );
            _appendStatsEntry( b , "writeLock" , coll.writeLock );

            _appendStatsEntry( b , "queries" , coll.queries );
            _appendStatsEntry( b , "getmore" , coll.getmore );
            _appendStatsEntry( b , "insert" , coll.insert );
            _appendStatsEntry( b , "update" , coll.update );
            _appendStatsEntry( b , "remove" , coll.remove );
            _appendStatsEntry( b , "commands" , coll.commands );

            bb.done();
        }
    }

    void Top::_appendStatsEntry( BSONObjBuilder& b , const char * statsName , const UsageData& map ) const {
        BSONObjBuilder bb( b.subobjStart( statsName ) );
        bb.appendNumber( "time" , map.time );
        bb.appendNumber( "count" , map.count );
        bb.done();
    }

    class TopCmd : public Command {
    public:
        TopCmd() : Command( "top", true ) {}

        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream& help ) const { help << "usage by collection, in micros "; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::top);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            {
                BSONObjBuilder b( result.subobjStart( "totals" ) );
                b.append( "note" , "all times in microseconds" );
                Top::global.append( b );
                b.done();
            }
            return true;
        }

    } topCmd;

    Top Top::global;

}
