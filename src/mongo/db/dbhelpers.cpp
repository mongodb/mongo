// dbhelpers.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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
#include "db.h"
#include "dbhelpers.h"
#include "json.h"
#include "btree.h"
#include "pdfile.h"
#include "oplog.h"
#include "ops/update.h"
#include "ops/delete.h"
#include "queryoptimizercursor.h"
#include "mongo/client/dbclientinterface.h"

#include <fstream>

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>

namespace mongo {

    void Helpers::ensureIndex(const char *ns, BSONObj keyPattern, bool unique, const char *name) {
        NamespaceDetails *d = nsdetails(ns);
        if( d == 0 )
            return;

        {
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                if( i.next().keyPattern().woCompare(keyPattern) == 0 )
                    return;
            }
        }

        if( d->nIndexes >= NamespaceDetails::NIndexesMax ) {
            problem() << "Helper::ensureIndex fails, MaxIndexes exceeded " << ns << '\n';
            return;
        }

        string system_indexes = cc().database()->name + ".system.indexes";

        BSONObjBuilder b;
        b.append("name", name);
        b.append("ns", ns);
        b.append("key", keyPattern);
        b.appendBool("unique", unique);
        BSONObj o = b.done();

        theDataFileMgr.insert(system_indexes.c_str(), o.objdata(), o.objsize());
    }

    /* fetch a single object from collection ns that matches query
       set your db SavedContext first
    */
    bool Helpers::findOne(const char *ns, const BSONObj &query, BSONObj& result, bool requireIndex) {
        DiskLoc loc = findOne( ns, query, requireIndex );
        if ( loc.isNull() )
            return false;
        result = loc.obj();
        return true;
    }

    /* fetch a single object from collection ns that matches query
       set your db SavedContext first
    */
    DiskLoc Helpers::findOne(const char *ns, const BSONObj &query, bool requireIndex) {
        shared_ptr<Cursor> c =
        NamespaceDetailsTransient::getCursor( ns, query, BSONObj(),
                                             requireIndex ?
                                             QueryPlanSelectionPolicy::indexOnly() :
                                             QueryPlanSelectionPolicy::any() );
        while( c->ok() ) {
            if ( c->currentMatches() && !c->getsetdup( c->currLoc() ) ) {
                return c->currLoc();
            }
            c->advance();
        }
        return DiskLoc();
    }

    bool Helpers::findById(Client& c, const char *ns, BSONObj query, BSONObj& result ,
                           bool * nsFound , bool * indexFound ) {
        Lock::assertAtLeastReadLocked(ns);
        Database *database = c.database();
        verify( database );
        NamespaceDetails *d = database->namespaceIndex.details(ns);
        if ( ! d )
            return false;
        if ( nsFound )
            *nsFound = 1;

        int idxNo = d->findIdIndex();
        if ( idxNo < 0 )
            return false;
        if ( indexFound )
            *indexFound = 1;

        IndexDetails& i = d->idx( idxNo );

        BSONObj key = i.getKeyFromQuery( query );

        DiskLoc loc = i.idxInterface().findSingle(i , i.head , key);
        if ( loc.isNull() )
            return false;
        result = loc.obj();
        return true;
    }

    DiskLoc Helpers::findById(NamespaceDetails *d, BSONObj idquery) {
        verify(d);
        int idxNo = d->findIdIndex();
        uassert(13430, "no _id index", idxNo>=0);
        IndexDetails& i = d->idx( idxNo );
        BSONObj key = i.getKeyFromQuery( idquery );
        return i.idxInterface().findSingle(i , i.head , key);
    }

    bool Helpers::isEmpty(const char *ns, bool doAuth) {
        Client::Context context(ns, dbpath, doAuth);
        shared_ptr<Cursor> c = DataFileMgr::findAll(ns);
        return !c->ok();
    }

    /* Get the first object from a collection.  Generally only useful if the collection
       only ever has a single object -- which is a "singleton collection.

       Returns: true if object exists.
    */
    bool Helpers::getSingleton(const char *ns, BSONObj& result) {
        Client::Context context(ns);

        shared_ptr<Cursor> c = DataFileMgr::findAll(ns);
        if ( !c->ok() ) {
            context.getClient()->curop()->done();
            return false;
        }

        result = c->current();
        context.getClient()->curop()->done();
        return true;
    }

    bool Helpers::getLast(const char *ns, BSONObj& result) {
        Client::Context ctx(ns);
        shared_ptr<Cursor> c = findTableScan(ns, reverseNaturalObj);
        if( !c->ok() )
            return false;
        result = c->current();
        return true;
    }

    void Helpers::upsert( const string& ns , const BSONObj& o, bool fromMigrate ) {
        BSONElement e = o["_id"];
        verify( e.type() );
        BSONObj id = e.wrap();

        OpDebug debug;
        Client::Context context(ns);
        updateObjects(ns.c_str(), o, /*pattern=*/id, /*upsert=*/true, /*multi=*/false , /*logtheop=*/true , debug, fromMigrate );
    }

    void Helpers::putSingleton(const char *ns, BSONObj obj) {
        OpDebug debug;
        Client::Context context(ns);
        updateObjects(ns, obj, /*pattern=*/BSONObj(), /*upsert=*/true, /*multi=*/false , /*logtheop=*/true , debug );
        context.getClient()->curop()->done();
    }

    void Helpers::putSingletonGod(const char *ns, BSONObj obj, bool logTheOp) {
        OpDebug debug;
        Client::Context context(ns);
        _updateObjects(/*god=*/true, ns, obj, /*pattern=*/BSONObj(), /*upsert=*/true, /*multi=*/false , logTheOp , debug );
        context.getClient()->curop()->done();
    }

    BSONObj Helpers::toKeyFormat( const BSONObj& o , BSONObj& key ) {
        BSONObjBuilder me;
        BSONObjBuilder k;

        BSONObjIterator i( o );
        while ( i.more() ) {
            BSONElement e = i.next();
            k.append( e.fieldName() , 1 );
            me.appendAs( e , "" );
        }
        key = k.obj();
        return me.obj();
    }

    long long Helpers::removeRange( const string& ns , const BSONObj& min , const BSONObj& max , bool yield , bool maxInclusive , RemoveCallback * callback, bool fromMigrate ) {
        BSONObj keya , keyb;
        BSONObj minClean = toKeyFormat( min , keya );
        BSONObj maxClean = toKeyFormat( max , keyb );
        verify( keya == keyb );

        Client::Context ctx(ns);

        shared_ptr<Cursor> c;
        auto_ptr<ClientCursor> cc;
        {
            NamespaceDetails* nsd = nsdetails( ns.c_str() );
            if ( ! nsd )
                return 0;
            
            int ii = nsd->findIndexByKeyPattern( keya );
            verify( ii >= 0 );
            
            IndexDetails& i = nsd->idx( ii );
            
            c.reset( BtreeCursor::make( nsd , ii , i , minClean , maxClean , maxInclusive, 1 ) );
            cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , c , ns ) );
            cc->setDoingDeletes( true );
        }

        long long num = 0;

        while ( cc->ok() ) {

            if ( yield && ! cc->yieldSometimes( ClientCursor::WillNeed) ) {
                // cursor got finished by someone else, so we're done
                cc.release(); // if the collection/db is dropped, cc may be deleted
                break;
            }

            if ( ! cc->ok() )
                break;

            DiskLoc rloc = cc->currLoc();

            if ( callback )
                callback->goingToDelete( cc->current() );

            cc->advance();
            c->prepareToTouchEarlierIterate();

            logOp( "d" , ns.c_str() , rloc.obj()["_id"].wrap() , 0 , 0 , fromMigrate );
            theDataFileMgr.deleteRecord(ns.c_str() , rloc.rec(), rloc);
            num++;

            c->recoverFromTouchingEarlierIterate();

            getDur().commitIfNeeded();


        }

        return num;
    }

    void Helpers::emptyCollection(const char *ns) {
        Client::Context context(ns);
        deleteObjects(ns, BSONObj(), false);
    }

    RemoveSaver::RemoveSaver( const string& a , const string& b , const string& why) : _out(0) {
        static int NUM = 0;

        _root = dbpath;
        if ( a.size() )
            _root /= a;
        if ( b.size() )
            _root /= b;
        verify( a.size() || b.size() );

        _file = _root;

        stringstream ss;
        ss << why << "." << terseCurrentTime(false) << "." << NUM++ << ".bson";
        _file /= ss.str();

    }

    RemoveSaver::~RemoveSaver() {
        if ( _out ) {
            _out->close();
            delete _out;
            _out = 0;
        }
    }

    void RemoveSaver::goingToDelete( const BSONObj& o ) {
        if ( ! _out ) {
            boost::filesystem::create_directories( _root );
            _out = new ofstream();
            _out->open( _file.string().c_str() , ios_base::out | ios_base::binary );
            if ( ! _out->good() ) {
                log( LL_WARNING ) << "couldn't create file: " << _file.string() << " for remove saving" << endl;
                delete _out;
                _out = 0;
                return;
            }

        }
        _out->write( o.objdata() , o.objsize() );
    }


} // namespace mongo
