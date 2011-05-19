// dbclient.cpp - connect to a Mongo database as a database, from C++

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "dbclient.h"
#include "../bson/util/builder.h"
#include "../db/jsobj.h"
#include "../db/json.h"
#include "../db/dbmessage.h"
#include "connpool.h"
#include "dbclient_rs.h"
#include "../util/background.h"

namespace mongo {

    // --------------------------------
    // ----- ReplicaSetMonitor ---------
    // --------------------------------

    // global background job responsible for checking every X amount of time
    class ReplicaSetMonitorWatcher : public BackgroundJob {
    public:
        ReplicaSetMonitorWatcher() : _safego("ReplicaSetMonitorWatcher::_safego") , _started(false) {}

        virtual string name() const { return "ReplicaSetMonitorWatcher"; }
        
        void safeGo() {
            // check outside of lock for speed
            if ( _started )
                return;
            
            scoped_lock lk( _safego );
            if ( _started )
                return;
            _started = true;

            go();
        }
    protected:
        void run() {
            log() << "starting" << endl;
            while ( ! inShutdown() ) {
                sleepsecs( 20 );
                try {
                    ReplicaSetMonitor::checkAll();
                }
                catch ( std::exception& e ) {
                    error() << "check failed: " << e.what() << endl;
                }
                catch ( ... ) {
                    error() << "unkown error" << endl;
                }
            }
        }

        mongo::mutex _safego;
        bool _started;

    } replicaSetMonitorWatcher;


    ReplicaSetMonitor::ReplicaSetMonitor( const string& name , const vector<HostAndPort>& servers )
        : _lock( "ReplicaSetMonitor instance" ) , _checkConnectionLock( "ReplicaSetMonitor check connection lock" ), _name( name ) , _master(-1), _nextSlave(0) {
        
        uassert( 13642 , "need at least 1 node for a replica set" , servers.size() > 0 );

        if ( _name.size() == 0 ) {
            warning() << "replica set name empty, first node: " << servers[0] << endl;
        }

        string errmsg;

        for ( unsigned i=0; i<servers.size(); i++ ) {

            bool haveAlready = false;
            for ( unsigned n = 0; n < _nodes.size() && ! haveAlready; n++ )
                haveAlready = ( _nodes[n].addr == servers[i] );
            if( haveAlready ) continue;

            auto_ptr<DBClientConnection> conn( new DBClientConnection( true , 0, 5.0 ) );
            if (!conn->connect( servers[i] , errmsg ) ) {
                log(1) << "error connecting to seed " << servers[i] << ": " << errmsg << endl;
                // skip seeds that don't work
                continue;
            }

            _nodes.push_back( Node( servers[i] , conn.release() ) );

            string maybePrimary;
            if (_checkConnection( _nodes[_nodes.size()-1].conn , maybePrimary, false)) {
                break;
            }
        }
    }

    ReplicaSetMonitor::~ReplicaSetMonitor() {
        for ( unsigned i=0; i<_nodes.size(); i++ )
            delete _nodes[i].conn;
        _nodes.clear();
        _master = -1;
    }

    ReplicaSetMonitorPtr ReplicaSetMonitor::get( const string& name , const vector<HostAndPort>& servers ) {
        scoped_lock lk( _setsLock );
        ReplicaSetMonitorPtr& m = _sets[name];
        if ( ! m )
            m.reset( new ReplicaSetMonitor( name , servers ) );

        replicaSetMonitorWatcher.safeGo();

        return m;
    }

    void ReplicaSetMonitor::checkAll() {
        set<string> seen;

        while ( true ) {
            ReplicaSetMonitorPtr m;
            {
                scoped_lock lk( _setsLock );
                for ( map<string,ReplicaSetMonitorPtr>::iterator i=_sets.begin(); i!=_sets.end(); ++i ) {
                    string name = i->first;
                    if ( seen.count( name ) )
                        continue;
                    LOG(1) << "checking replica set: " << name << endl;
                    seen.insert( name );
                    m = i->second;
                    break;
                }
            }

            if ( ! m )
                break;

            m->check();
        }


    }

    void ReplicaSetMonitor::setConfigChangeHook( ConfigChangeHook hook ) {
        massert( 13610 , "ConfigChangeHook already specified" , _hook == 0 );
        _hook = hook;
    }
    
    string ReplicaSetMonitor::getServerAddress() const {
        StringBuilder ss;
        if ( _name.size() )
            ss << _name << "/";

        {
            scoped_lock lk( _lock );
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                if ( i > 0 )
                    ss << ",";
                ss << _nodes[i].addr.toString();
            }
        }
        return ss.str();
    }

    bool ReplicaSetMonitor::contains( const string& server ) const {
        scoped_lock lk( _lock );
        for ( unsigned i=0; i<_nodes.size(); i++ ) {
            if ( _nodes[i].addr == server )
                return true;
        }
        return false;
    }
    

    void ReplicaSetMonitor::notifyFailure( const HostAndPort& server ) {
        scoped_lock lk( _lock );
        if ( _master >= 0 && _master < (int)_nodes.size() ) {
            if ( server == _nodes[_master].addr ) {
                _nodes[_master].ok = false; 
                _master = -1;
            }
        }
    }



    HostAndPort ReplicaSetMonitor::getMaster() {
        {
            scoped_lock lk( _lock );
            if ( _master >= 0 && _nodes[_master].ok )
                return _nodes[_master].addr;
        }
        
        _check();

        scoped_lock lk( _lock );
        uassert( 10009 , str::stream() << "ReplicaSetMonitor no master found for set: " << _name , _master >= 0 );
        return _nodes[_master].addr;
    }
    
    HostAndPort ReplicaSetMonitor::getSlave( const HostAndPort& prev ) {
        // make sure its valid 
        if ( prev.port() > 0 ) {
            scoped_lock lk( _lock );
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                if ( prev != _nodes[i].addr ) 
                    continue;

                if ( _nodes[i].ok ) 
                    return prev;
                break;
            }
        }
        
        return getSlave();
    }

    HostAndPort ReplicaSetMonitor::getSlave() {

        {
            scoped_lock lk( _lock );
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                _nextSlave = ( _nextSlave + 1 ) % _nodes.size();
                if ( _nextSlave == _master )
                    continue;
                if ( _nodes[ _nextSlave ].ok )
                    return _nodes[ _nextSlave ].addr;
            }
        }

        return _nodes[ 0 ].addr;
    }

    /**
     * notify the monitor that server has faild
     */
    void ReplicaSetMonitor::notifySlaveFailure( const HostAndPort& server ) {
        int x = _find( server );
        if ( x >= 0 ) {
            scoped_lock lk( _lock );
            _nodes[x].ok = false;
        }
    }

    void ReplicaSetMonitor::_checkStatus(DBClientConnection *conn) {
        BSONObj status;

        if (!conn->runCommand("admin", BSON("replSetGetStatus" << 1), status) ||
                !status.hasField("members") ||
                status["members"].type() != Array) {
            return;
        }

        BSONObjIterator hi(status["members"].Obj());
        while (hi.more()) {
            BSONObj member = hi.next().Obj();
            string host = member["name"].String();

            int m = -1;
            if ((m = _find(host)) <= 0) {
                continue;
            }

            double state = member["state"].Number();
            if (member["health"].Number() == 1 && (state == 1 || state == 2)) {
                scoped_lock lk( _lock );
                _nodes[m].ok = true;
            }
            else {
                scoped_lock lk( _lock );
                _nodes[m].ok = false;
            }
        }
    }

    void ReplicaSetMonitor::_checkHosts( const BSONObj& hostList, bool& changed ) {
        BSONObjIterator hi(hostList);
        while ( hi.more() ) {
            string toCheck = hi.next().String();

            if ( _find( toCheck ) >= 0 )
                continue;

            HostAndPort h( toCheck );
            DBClientConnection * newConn = new DBClientConnection( true, 0, 5.0 );
            string temp;
            newConn->connect( h , temp );
            {
                scoped_lock lk( _lock );
                if ( _find_inlock( toCheck ) >= 0 ) {
                    // we need this check inside the lock so there isn't thread contention on adding to vector
                    continue;
                }
                _nodes.push_back( Node( h , newConn ) );
            }
            log() << "updated set (" << _name << ") to: " << getServerAddress() << endl;
            changed = true;
        }
    }
    
    

    bool ReplicaSetMonitor::_checkConnection( DBClientConnection * c , string& maybePrimary , bool verbose ) {
        scoped_lock lk( _checkConnectionLock );
        bool isMaster = false;
        bool changed = false;
        try {
            BSONObj o;
            c->isMaster(isMaster, &o);

            log( ! verbose ) << "ReplicaSetMonitor::_checkConnection: " << c->toString() << ' ' << o << endl;

            // add other nodes
            if ( o["hosts"].type() == Array ) {
                if ( o["primary"].type() == String )
                    maybePrimary = o["primary"].String();

                _checkHosts(o["hosts"].Obj(), changed);
            }
            if (o.hasField("passives") && o["passives"].type() == Array) {
                _checkHosts(o["passives"].Obj(), changed);
            }

            _checkStatus(c);
        }
        catch ( std::exception& e ) {
            log( ! verbose ) << "ReplicaSetMonitor::_checkConnection: caught exception " << c->toString() << ' ' << e.what() << endl;
        }

        if ( changed && _hook )
            _hook( this );

        return isMaster;
    }

    void ReplicaSetMonitor::_check() {

        bool triedQuickCheck = false;

        LOG(1) <<  "_check : " << getServerAddress() << endl;

        for ( int retry = 0; retry < 2; retry++ ) {
            for ( unsigned i=0; i<_nodes.size(); i++ ) {
                DBClientConnection * c;
                {
                    scoped_lock lk( _lock );
                    c = _nodes[i].conn;
                }

                string maybePrimary;
                if ( _checkConnection( c , maybePrimary , retry ) ) {
                    _master = i;
                    return;
                }

                if ( ! triedQuickCheck && maybePrimary.size() ) {
                    int x = _find( maybePrimary );
                    if ( x >= 0 ) {
                        triedQuickCheck = true;
                        string dummy;
                        DBClientConnection * testConn;
                        {
                            scoped_lock lk( _lock );
                            testConn = _nodes[x].conn;
                        }
                        if ( _checkConnection( testConn , dummy , false ) ) {
                            _master = x;
                            return;
                        }
                    }
                }

            }
            sleepsecs(1);
        }

    }

    void ReplicaSetMonitor::check() {
        // first see if the current master is fine
        if ( _master >= 0 ) {
            string temp;
            if ( _checkConnection( _nodes[_master].conn , temp , false ) ) {
                // current master is fine, so we're done
                return;
            }
        }

        // we either have no master, or the current is dead
        _check();
    }

    int ReplicaSetMonitor::_find( const string& server ) const {
        scoped_lock lk( _lock );
        return _find_inlock( server );
    }

    int ReplicaSetMonitor::_find_inlock( const string& server ) const {
        for ( unsigned i=0; i<_nodes.size(); i++ )
            if ( _nodes[i].addr == server )
                return i;
        return -1;
    }


    int ReplicaSetMonitor::_find( const HostAndPort& server ) const {
        scoped_lock lk( _lock );
        for ( unsigned i=0; i<_nodes.size(); i++ )
            if ( _nodes[i].addr == server )
                return i;
        return -1;
    }


    mongo::mutex ReplicaSetMonitor::_setsLock( "ReplicaSetMonitor" );
    map<string,ReplicaSetMonitorPtr> ReplicaSetMonitor::_sets;
    ReplicaSetMonitor::ConfigChangeHook ReplicaSetMonitor::_hook;
    // --------------------------------
    // ----- DBClientReplicaSet ---------
    // --------------------------------

    DBClientReplicaSet::DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers )
        : _monitor( ReplicaSetMonitor::get( name , servers ) ) {
    }

    DBClientReplicaSet::~DBClientReplicaSet() {
    }

    DBClientConnection * DBClientReplicaSet::checkMaster() {
        HostAndPort h = _monitor->getMaster();

        if ( h == _masterHost ) {
            // a master is selected.  let's just make sure connection didn't die
            if ( ! _master->isFailed() )
                return _master.get();
            _monitor->notifyFailure( _masterHost );
        }

        _masterHost = _monitor->getMaster();
        _master.reset( new DBClientConnection( true , this ) );
        string errmsg;
        if ( ! _master->connect( _masterHost , errmsg ) ) {
            _monitor->notifyFailure( _masterHost );
            uasserted( 13639 , str::stream() << "can't connect to new replica set master [" << _masterHost.toString() << "] err: " << errmsg );
        }
        _auth( _master.get() );
        return _master.get();
    }

    DBClientConnection * DBClientReplicaSet::checkSlave() {
        HostAndPort h = _monitor->getSlave( _slaveHost );

        if ( h == _slaveHost ) {
            if ( ! _slave->isFailed() )
                return _slave.get();
            _monitor->notifySlaveFailure( _slaveHost );
        }
        
        _slaveHost = _monitor->getSlave();
        _slave.reset( new DBClientConnection( true , this ) );
        _slave->connect( _slaveHost );
        _auth( _slave.get() );
        return _slave.get();
    }


    void DBClientReplicaSet::_auth( DBClientConnection * conn ) {
        for ( list<AuthInfo>::iterator i=_auths.begin(); i!=_auths.end(); ++i ) {
            const AuthInfo& a = *i;
            string errmsg;
            if ( ! conn->auth( a.dbname , a.username , a.pwd , errmsg, a.digestPassword ) )
                warning() << "cached auth failed for set: " << _monitor->getName() << " db: " << a.dbname << " user: " << a.username << endl;

        }

    }

    DBClientConnection& DBClientReplicaSet::masterConn() {
        return *checkMaster();
    }

    DBClientConnection& DBClientReplicaSet::slaveConn() {
        return *checkSlave();
    }

    bool DBClientReplicaSet::connect() {
        try {
            checkMaster();
        }
        catch (AssertionException&) {
            if (_master && _monitor) {
                _monitor->notifyFailure(_masterHost);
            }
            return false;
        }
        return true;
    }

    bool DBClientReplicaSet::auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword ) {
        DBClientConnection * m = checkMaster();

        // first make sure it actually works
        if( ! m->auth(dbname, username, pwd, errmsg, digestPassword ) )
            return false;

        // now that it does, we should save so that for a new node we can auth
        _auths.push_back( AuthInfo( dbname , username , pwd , digestPassword ) );
        return true;
    }

    // ------------- simple functions -----------------

    void DBClientReplicaSet::insert( const string &ns , BSONObj obj ) {
        checkMaster()->insert(ns, obj);
    }

    void DBClientReplicaSet::insert( const string &ns, const vector< BSONObj >& v ) {
        checkMaster()->insert(ns, v);
    }

    void DBClientReplicaSet::remove( const string &ns , Query obj , bool justOne ) {
        checkMaster()->remove(ns, obj, justOne);
    }

    void DBClientReplicaSet::update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi ) {
        return checkMaster()->update(ns, query, obj, upsert,multi);
    }

    auto_ptr<DBClientCursor> DBClientReplicaSet::query(const string &ns, Query query, int nToReturn, int nToSkip,
            const BSONObj *fieldsToReturn, int queryOptions, int batchSize) {

        if ( queryOptions & QueryOption_SlaveOk ) {
            // we're ok sending to a slave
            // we'll try 2 slaves before just using master
            // checkSlave will try a different slave automatically after a failure
            for ( int i=0; i<3; i++ ) {
                try {
                    return checkSlave()->query(ns,query,nToReturn,nToSkip,fieldsToReturn,queryOptions,batchSize);
                }
                catch ( DBException &e ) {
                    LOG(1) << "can't query replica set slave " << i << " : " << _slaveHost << causedBy( e ) << endl;
                }
            }
        }

        return checkMaster()->query(ns,query,nToReturn,nToSkip,fieldsToReturn,queryOptions,batchSize);
    }

    BSONObj DBClientReplicaSet::findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn, int queryOptions) {
        if ( queryOptions & QueryOption_SlaveOk ) {
            // we're ok sending to a slave
            // we'll try 2 slaves before just using master
            // checkSlave will try a different slave automatically after a failure
            for ( int i=0; i<3; i++ ) {
                try {
                    return checkSlave()->findOne(ns,query,fieldsToReturn,queryOptions);
                }
                catch ( DBException &e ) {
                	LOG(1) << "can't findone replica set slave " << i << " : " << _slaveHost << causedBy( e ) << endl;
                }
            }
        }

        return checkMaster()->findOne(ns,query,fieldsToReturn,queryOptions);
    }

    void DBClientReplicaSet::killCursor( long long cursorID ) {
        // we should neve call killCursor on a replica set conncetion
        // since we don't know which server it belongs to
        // can't assume master because of slave ok
        // and can have a cursor survive a master change
        assert(0);
    }

    void DBClientReplicaSet::isntMaster() { 
        log() << "got not master for: " << _masterHost << endl;
        _monitor->notifyFailure( _masterHost );
        _master.reset(); 
    }

    DBClientBase* DBClientReplicaSet::callLazy( Message& toSend ) {
        if ( toSend.operation() == dbQuery ) {
            // TODO: might be possible to do this faster by changing api
            DbMessage dm( toSend );
            QueryMessage qm( dm );
            if ( qm.queryOptions & QueryOption_SlaveOk ) {
                for ( int i=0; i<3; i++ ) {
                    try {
                        return checkSlave()->callLazy( toSend );
                    }
                    catch ( DBException &e ) {
                    	LOG(1) << "can't callLazy replica set slave " << i << " : " << _slaveHost << causedBy( e ) << endl;
                    }
                }
            }
        }

        return checkMaster()->callLazy( toSend );
    }

    bool DBClientReplicaSet::call( Message &toSend, Message &response, bool assertOk , string * actualServer ) {
        if ( toSend.operation() == dbQuery ) {
            // TODO: might be possible to do this faster by changing api
            DbMessage dm( toSend );
            QueryMessage qm( dm );
            if ( qm.queryOptions & QueryOption_SlaveOk ) {
                for ( int i=0; i<3; i++ ) {
                    try {
                        DBClientConnection* s = checkSlave();
                        if ( actualServer )
                            *actualServer = s->getServerAddress();
                        return s->call( toSend , response , assertOk );
                    }
                    catch ( DBException &e ) {
                    	LOG(1) << "can't call replica set slave " << i << " : " << _slaveHost << causedBy( e ) << endl;
                        if ( actualServer )
                            *actualServer = "";
                    }
                }
            }
        }
        
        DBClientConnection* m = checkMaster();
        if ( actualServer )
            *actualServer = m->getServerAddress();
        return m->call( toSend , response , assertOk );
    }

}
