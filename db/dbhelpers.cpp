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
#include "query.h"
#include "json.h"
#include "queryoptimizer.h"
#include "btree.h"
#include "pdfile.h"
#include "oplog.h"

namespace mongo {

    CursorIterator::CursorIterator( shared_ptr<Cursor> c , BSONObj filter )
        : _cursor( c ){
            if ( ! filter.isEmpty() )
                _matcher.reset( new CoveredIndexMatcher( filter , BSONObj() ) );
            _advance();
    }

    BSONObj CursorIterator::next(){
        BSONObj o = _o;
        _advance();
        return o;
    }
    
    bool CursorIterator::hasNext(){
        return ! _o.isEmpty();
    }

    void CursorIterator::_advance(){
        if ( ! _cursor->ok() ){
            _o = BSONObj();
            return;
        }
        
        while ( _cursor->ok() ){
            _o = _cursor->current();
            _cursor->advance();
            if ( _matcher.get() == 0 || _matcher->matches( _o ) )
                return;
        }

        _o = BSONObj();
    }

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

    class FindOne : public QueryOp {
    public:
        FindOne( bool requireIndex ) : requireIndex_( requireIndex ) {}
        virtual void _init() {
            if ( requireIndex_ && strcmp( qp().indexKey().firstElement().fieldName(), "$natural" ) == 0 )
                throw MsgAssertionException( 9011 , "Not an index cursor" );
            c_ = qp().newCursor();
            if ( !c_->ok() ) {
                setComplete();
            }
        }
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }
            if ( matcher()->matches( c_->currKey(), c_->currLoc() ) ) {
                one_ = c_->current();
                loc_ = c_->currLoc();
                setStop();
            } else {
                c_->advance();
            }
        }
        virtual long long nscanned() {
            assert( c_.get() );
            return c_->nscanned();
        }
        virtual bool mayRecordPlan() const { return false; }
        virtual QueryOp *_createChild() const { return new FindOne( requireIndex_ ); }
        BSONObj one() const { return one_; }
        DiskLoc loc() const { return loc_; }
    private:
        bool requireIndex_;
        shared_ptr<Cursor> c_;
        BSONObj one_;
        DiskLoc loc_;
    };
    
    /* fetch a single object from collection ns that matches query 
       set your db SavedContext first
    */
    bool Helpers::findOne(const char *ns, const BSONObj &query, BSONObj& result, bool requireIndex) { 
        MultiPlanScanner s( ns, query, BSONObj(), 0, !requireIndex );
        FindOne original( requireIndex );
        shared_ptr< FindOne > res = s.runOp( original );
        if ( ! res->complete() )
            throw MsgAssertionException( res->exception() );
        if ( res->one().isEmpty() )
            return false;
        result = res->one();
        return true;
    }

    /* fetch a single object from collection ns that matches query 
       set your db SavedContext first
    */
    DiskLoc Helpers::findOne(const char *ns, const BSONObj &query, bool requireIndex) { 
        MultiPlanScanner s( ns, query, BSONObj(), 0, !requireIndex );
        FindOne original( requireIndex );
        shared_ptr< FindOne > res = s.runOp( original );
        if ( ! res->complete() )
            throw MsgAssertionException( res->exception() );
        return res->loc();
    }

    auto_ptr<CursorIterator> Helpers::find( const char *ns , BSONObj query , bool requireIndex ){
        uassert( 10047 ,  "requireIndex not supported in Helpers::find yet" , ! requireIndex );
        auto_ptr<CursorIterator> i;
        i.reset( new CursorIterator( DataFileMgr::findAll( ns ) , query ) );
        return i;
    }
    
    bool Helpers::findById(Client& c, const char *ns, BSONObj query, BSONObj& result ,
                           bool * nsFound , bool * indexFound ){
        dbMutex.assertAtLeastReadLocked();
        Database *database = c.database();
        assert( database );
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
        
        DiskLoc loc = i.head.btree()->findSingle( i , i.head , key );
        if ( loc.isNull() )
            return false;
        result = loc.obj();
        return true;
    }

     DiskLoc Helpers::findById(NamespaceDetails *d, BSONObj idquery) {
         int idxNo = d->findIdIndex();
         uassert(13430, "no _id index", idxNo>=0);
         IndexDetails& i = d->idx( idxNo );        
         BSONObj key = i.getKeyFromQuery( idquery );
         return i.head.btree()->findSingle( i , i.head , key );
    }

    bool Helpers::isEmpty(const char *ns) {
        Client::Context context(ns);
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
        if ( !c->ok() )
            return false;

        result = c->current();
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

    void Helpers::upsert( const string& ns , const BSONObj& o ){
        BSONElement e = o["_id"];
        assert( e.type() );
        BSONObj id = e.wrap();
        
        OpDebug debug;
        Client::Context context(ns);
        updateObjects(ns.c_str(), o, /*pattern=*/id, /*upsert=*/true, /*multi=*/false , /*logtheop=*/true , debug );
    }

    void Helpers::putSingleton(const char *ns, BSONObj obj) {
        OpDebug debug;
        Client::Context context(ns);
        updateObjects(ns, obj, /*pattern=*/BSONObj(), /*upsert=*/true, /*multi=*/false , /*logtheop=*/true , debug );
    }

    void Helpers::putSingletonGod(const char *ns, BSONObj obj, bool logTheOp) {
        OpDebug debug;
        Client::Context context(ns);
        _updateObjects(/*god=*/true, ns, obj, /*pattern=*/BSONObj(), /*upsert=*/true, /*multi=*/false , logTheOp , debug );
    }

    BSONObj Helpers::toKeyFormat( const BSONObj& o , BSONObj& key ){
        BSONObjBuilder me;
        BSONObjBuilder k;

        BSONObjIterator i( o );
        while ( i.more() ){
            BSONElement e = i.next();
            k.append( e.fieldName() , 1 );
            me.appendAs( e , "" );
        }
        key = k.obj();
        return me.obj();
    }
    
    long long Helpers::removeRange( const string& ns , const BSONObj& min , const BSONObj& max , bool yield , bool maxInclusive , RemoveCallback * callback ){
        BSONObj keya , keyb;
        BSONObj minClean = toKeyFormat( min , keya );
        BSONObj maxClean = toKeyFormat( max , keyb );
        assert( keya == keyb );

        Client::Context ctx(ns);
        NamespaceDetails* nsd = nsdetails( ns.c_str() );
        if ( ! nsd )
            return 0;

        int ii = nsd->findIndexByKeyPattern( keya );
        assert( ii >= 0 );
        
        long long num = 0;
        
        IndexDetails& i = nsd->idx( ii );

        shared_ptr<Cursor> c( new BtreeCursor( nsd , ii , i , minClean , maxClean , maxInclusive, 1 ) );
        auto_ptr<ClientCursor> cc( new ClientCursor( QueryOption_NoCursorTimeout , c , ns ) );
        cc->setDoingDeletes( true );
        
        while ( c->ok() ){
            DiskLoc rloc = c->currLoc();
            BSONObj key = c->currKey();

            if ( callback )
                callback->goingToDelete( c->current() );
            
            c->advance();
            c->noteLocation();
            
            logOp( "d" , ns.c_str() , rloc.obj()["_id"].wrap() );
            theDataFileMgr.deleteRecord(ns.c_str() , rloc.rec(), rloc);
            num++;

            c->checkLocation();

            if ( yield && ! cc->yieldSometimes() ){
                // cursor got finished by someone else, so we're done
                break;
            }
        }

        return num;
    }

    void Helpers::emptyCollection(const char *ns) {
        Client::Context context(ns);
        deleteObjects(ns, BSONObj(), false);
    }

    DbSet::~DbSet() {
        if ( name_.empty() )
            return;
        try {
            Client::Context c( name_.c_str() );
            if ( nsdetails( name_.c_str() ) ) {
                string errmsg;
                BSONObjBuilder result;
                dropCollection( name_, errmsg, result );
            }
        } catch ( ... ) {
            problem() << "exception cleaning up DbSet" << endl;
        }
    }
    
    void DbSet::reset( const string &name, const BSONObj &key ) {
        if ( !name.empty() )
            name_ = name;
        if ( !key.isEmpty() )
            key_ = key.getOwned();
        Client::Context c( name_.c_str() );
        if ( nsdetails( name_.c_str() ) ) {
            Helpers::emptyCollection( name_.c_str() );
        } else {
            string err;
            massert( 10303 ,  err, userCreateNS( name_.c_str(), fromjson( "{autoIndexId:false}" ), err, false ) );
        }
        Helpers::ensureIndex( name_.c_str(), key_, true, "setIdx" );            
    }
    
    bool DbSet::get( const BSONObj &obj ) const {
        Client::Context c( name_.c_str() );
        BSONObj temp;
        return Helpers::findOne( name_.c_str(), obj, temp, true );
    }
    
    void DbSet::set( const BSONObj &obj, bool val ) {
        Client::Context c( name_.c_str() );
        if ( val ) {
            try {
                BSONObj k = obj;
                theDataFileMgr.insertWithObjMod( name_.c_str(), k, false );
            } catch ( DBException& ) {
                // dup key - already in set
            }
        } else {
            deleteObjects( name_.c_str(), obj, true, false, false );
        }                        
    }

    RemoveSaver::RemoveSaver( const string& a , const string& b , const string& why) : _out(0){
        static int NUM = 0;
        
        _root = dbpath;
        if ( a.size() )
            _root /= a;
        if ( b.size() )
            _root /= b;
        assert( a.size() || b.size() );
        
        _file = _root;
        
        stringstream ss;
        ss << why << "." << terseCurrentTime(false) << "." << NUM++ << ".bson";
        _file /= ss.str();

    }
    
    RemoveSaver::~RemoveSaver(){
        if ( _out ){
            _out->close();
            delete _out;
            _out = 0;
        }
    }
    
    void RemoveSaver::goingToDelete( const BSONObj& o ){
        if ( ! _out ){
            create_directories( _root );
            _out = new ofstream();
            _out->open( _file.string().c_str() , ios_base::out | ios_base::binary );
            if ( ! _out->good() ){
                log( LL_WARNING ) << "couldn't create file: " << _file.string() << " for remove saving" << endl;
                delete _out;
                _out = 0;
                return;
            }
            
        }
        _out->write( o.objdata() , o.objsize() );
    }
    
        
} // namespace mongo
