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

#include "stdafx.h"
#include "db.h"
#include "dbhelpers.h"
#include "query.h"
#include "json.h"
#include "queryoptimizer.h"
#include "btree.h"

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

    class FindOne : public QueryOp {
    public:
        FindOne( bool requireIndex ) : requireIndex_( requireIndex ) {}
        virtual void init() {
            if ( requireIndex_ && strcmp( qp().indexKey().firstElement().fieldName(), "$natural" ) == 0 )
                throw MsgAssertionException( "Not an index cursor" );
            c_ = qp().newCursor();
            if ( !c_->ok() )
                setComplete();
            else
                matcher_.reset( new KeyValJSMatcher( qp().query(), qp().indexKey() ) );
        }
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }
            if ( matcher_->matches( c_->currKey(), c_->currLoc() ) ) {
                one_ = c_->current();
                setComplete();
            } else {
                c_->advance();
            }
        }
        virtual bool mayRecordPlan() const { return false; }
        virtual QueryOp *clone() const { return new FindOne( requireIndex_ ); }
        BSONObj one() const { return one_; }
    private:
        bool requireIndex_;
        auto_ptr< Cursor > c_;
        auto_ptr< KeyValJSMatcher > matcher_;
        BSONObj one_;
    };
    
    /* fetch a single object from collection ns that matches query 
       set your db SavedContext first
    */
    bool Helpers::findOne(const char *ns, BSONObj query, BSONObj& result, bool requireIndex) { 
        QueryPlanSet s( ns, query, BSONObj(), 0, !requireIndex );
        FindOne original( requireIndex );
        shared_ptr< FindOne > res = s.runOp( original );
        massert( res->exceptionMessage(), res->complete() );
        if ( res->one().isEmpty() )
            return false;
        result = res->one();
        return true;
    }

    int Helpers::findById(const char *ns, BSONObj query, BSONObj& result ){
        NamespaceDetails *d = nsdetails(ns);
        if ( ! d )
            return -1;
        int idxNo = d->findIdIndex();
        if ( idxNo < 0 )
            return -1;
        IndexDetails& i = d->idx( idxNo );
        
        BSONObj key = i.getKeyFromQuery( query );
        
        DiskLoc loc = i.head.btree()->findSingle( i , i.head , key );
        if ( loc.isNull() )
            return false;
        result = loc.obj();
        return true;
    }

    /* Get the first object from a collection.  Generally only useful if the collection
       only ever has a single object -- which is a "singleton collection.

       Returns: true if object exists.
    */
    bool Helpers::getSingleton(const char *ns, BSONObj& result) {
        DBContext context(ns);

        auto_ptr<Cursor> c = DataFileMgr::findAll(ns);
        if ( !c->ok() )
            return false;

        result = c->current();
        return true;
    }

    void Helpers::putSingleton(const char *ns, BSONObj obj) {
        DBContext context(ns);
        stringstream ss;
        updateObjects(ns, obj, /*pattern=*/BSONObj(), /*upsert=*/true, /*multi=*/false, ss, /*logop=*/true );
    }

    void Helpers::emptyCollection(const char *ns) {
        DBContext context(ns);
        deleteObjects(ns, BSONObj(), false);
    }

    DbSet::~DbSet() {
        if ( name_.empty() )
            return;
        try {
            DBContext c( name_.c_str() );
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
        DBContext c( name_.c_str() );
        if ( nsdetails( name_.c_str() ) ) {
            Helpers::emptyCollection( name_.c_str() );
        } else {
            string err;
            massert( err, userCreateNS( name_.c_str(), fromjson( "{autoIndexId:false}" ), err, false ) );
        }
        Helpers::ensureIndex( name_.c_str(), key_, true, "setIdx" );            
    }
    
    bool DbSet::get( const BSONObj &obj ) const {
        DBContext c( name_.c_str() );
        BSONObj temp;
        return Helpers::findOne( name_.c_str(), obj, temp, true );
    }
    
    void DbSet::set( const BSONObj &obj, bool val ) {
        DBContext c( name_.c_str() );
        if ( val ) {
            try {
                BSONObj k = obj;
                theDataFileMgr.insert( name_.c_str(), k, false );
            } catch ( DBException& ) {
                // dup key - already in set
            }
        } else {
            deleteObjects( name_.c_str(), obj, true, false, false );
        }                        
    }
        
} // namespace mongo
