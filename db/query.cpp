// query.cpp

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
#include "query.h"
#include "pdfile.h"
#include "jsobjmanipulator.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "javajs.h"
#include "json.h"
#include "repl.h"
#include "replset.h"
#include "scanandorder.h"
#include "security.h"
#include "curop.h"
#include "commands.h"
#include "queryoptimizer.h"
#include "lasterror.h"

namespace mongo {

    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    const int MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

//ns->query->DiskLoc
    LRUishMap<BSONObj,DiskLoc,5> lrutest(123);

    extern bool useCursors;
    extern bool useHints;

    // Just try to identify best plan.
    class DeleteOp : public QueryOp {
    public:
        DeleteOp( bool justOne, int& bestCount ) :
        justOne_( justOne ),
        count_(),
        bestCount_( bestCount ),
        nScanned_() {
        }
        virtual void init() {
            c_ = qp().newCursor();
            matcher_.reset( new KeyValJSMatcher( qp().query(), qp().indexKey() ) );
        }
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }
            
            DiskLoc rloc = c_->currLoc();
            
            bool deep;
            if ( matcher_->matches(c_->currKey(), rloc, &deep) ) {
                if ( !deep || !c_->getsetdup(rloc) )
                    ++count_;
            }

            c_->advance();
            ++nScanned_;
            if ( count_ > bestCount_ )
                bestCount_ = count_;
            
            if ( count_ > 0 ) {
                if ( justOne_ )
                    setComplete();
                else if ( nScanned_ >= 100 && count_ == bestCount_ )
                    setComplete();
            }
        }
        virtual bool mayRecordPlan() const { return !justOne_; }
        virtual QueryOp *clone() const {
            return new DeleteOp( justOne_, bestCount_ );
        }
        auto_ptr< Cursor > newCursor() const { return qp().newCursor(); }
    private:
        bool justOne_;
        int count_;
        int &bestCount_;
        long long nScanned_;
        auto_ptr< Cursor > c_;
        auto_ptr< KeyValJSMatcher > matcher_;
    };
    
    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
    */
    int deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool logop, bool god) {
        if ( strstr(ns, ".system.") && !god ) {
            /* note a delete from system.indexes would corrupt the db 
               if done here, as there are pointers into those objects in 
               NamespaceDetails.
            */
            if( strstr(ns, ".system.users") )
                ;
            else {
                out() << "ERROR: attempt to delete in system namespace " << ns << endl;
                return -1;
            }
        }

        int nDeleted = 0;
        QueryPlanSet s( ns, pattern, BSONObj() );
        int best = 0;
        DeleteOp original( justOne, best );
        shared_ptr< DeleteOp > bestOp = s.runOp( original );
        auto_ptr< Cursor > c = bestOp->newCursor();
        
        if( !c->ok() )
            return nDeleted;

        KeyValJSMatcher matcher(pattern, c->indexKeyPattern());

        do {
            DiskLoc rloc = c->currLoc();
            
            bool deep;
            if ( !matcher.matches(c->currKey(), rloc, &deep) ) {
                c->advance(); // advance must be after noMoreMatches() because it uses currKey()
            }
            else {
                c->advance(); // must advance before deleting as the next ptr will die
                assert( !deep || !c->getsetdup(rloc) ); // can't be a dup, we deleted it!
                if ( !justOne )
                    c->noteLocation();

                if ( logop ) {
                    BSONElement e;
                    if( BSONObj( rloc.rec() ).getObjectID( e ) ) {
                        BSONObjBuilder b;
                        b.append( e );
                        bool replJustOne = true;
                        logOp( "d", ns, b.done(), 0, &replJustOne );
                    } else {
                        problem() << "deleted object without id, not logging" << endl;
                    }
                }
                theDataFileMgr.deleteRecord(ns, rloc.rec(), rloc);
                nDeleted++;
                if ( justOne )
                    break;
                c->checkLocation();
            }
        } while ( c->ok() );

        return nDeleted;
    }

    class EmbeddedBuilder {
    public:
        EmbeddedBuilder( BSONObjBuilder *b ) {
            builders_.push_back( make_pair( "", b ) );
        }
        // It is assumed that the calls to prepareContext will be made with the 'name'
        // parameter in lex ascending order.
        void prepareContext( string &name ) {
            int i = 1, n = builders_.size();
            while( i < n && name.substr( 0, builders_[ i ].first.length() ) == builders_[ i ].first ) {
                name = name.substr( builders_[ i ].first.length() + 1 );
                ++i;
            }
            for( int j = n - 1; j >= i; --j ) {
                popBuilder();
            }
            for( string next = splitDot( name ); !next.empty(); next = splitDot( name ) ) {
                addBuilder( next );
            }
        }
        void appendAs( const BSONElement &e, string name ) {
            if ( e.type() == Object && e.valuesize() == 5 ) { // empty object -- this way we can add to it later
                string dummyName = name + ".foo";
                prepareContext( dummyName );
                return;
            }
            prepareContext( name );
            back()->appendAs( e, name.c_str() );
        }
        BufBuilder &subarrayStartAs( string name ) {
            prepareContext( name );
            return back()->subarrayStart( name.c_str() );
        }
        void done() {
            while( !builderStorage_.empty() )
                popBuilder();
        }
        static string splitDot( string & str ) {
            size_t pos = str.find( '.' );
            if ( pos == string::npos )
                return "";
            string ret = str.substr( 0, pos );
            str = str.substr( pos + 1 );
            return ret;
        }
    private:
        void addBuilder( const string &name ) {
            shared_ptr< BSONObjBuilder > newBuilder( new BSONObjBuilder( back()->subobjStart( name.c_str() ) ) );
            builders_.push_back( make_pair( name, newBuilder.get() ) );
            builderStorage_.push_back( newBuilder );
        }
        void popBuilder() {
            back()->done();
            builders_.pop_back();
            builderStorage_.pop_back();
        }
        BSONObjBuilder *back() { return builders_.back().second; }
        vector< pair< string, BSONObjBuilder * > > builders_;
        vector< shared_ptr< BSONObjBuilder > > builderStorage_;
    };
    
    struct Mod {
        enum Op { INC, SET, PUSH } op;
        const char *fieldName;
        double *ndouble;
        int *nint;
        BSONElement elt;
        int pushStartSize;
        void setn(double n) const {
            if ( ndouble ) *ndouble = n;
            else *nint = (int) n;
        }
        double getn() const {
            return ndouble ? *ndouble : *nint;
        }
        bool operator<( const Mod &other ) const {
            return strcmp( fieldName, other.fieldName ) < 0;
        }
    };

    class ModSet {
        vector< Mod > mods_;
        void sortMods() {
            sort( mods_.begin(), mods_.end() );
        }
        static void extractFields( map< string, BSONElement > &fields, const BSONElement &top, const string &base );
        int compare( const vector< Mod >::iterator &m, map< string, BSONElement >::iterator &p, const map< string, BSONElement >::iterator &pEnd ) const {
            bool mDone = ( m == mods_.end() );
            bool pDone = ( p == pEnd );
            if ( mDone && pDone )
                return 0;
            // If one iterator is done we want to read from the other one, so say the other one is lower.
            if ( mDone )
                return 1;
            if ( pDone )
                return -1;
            return strcmp( m->fieldName, p->first.c_str() );
        }
        bool mayAddEmbedded( map< string, BSONElement > &existing, string right ) {
            for( string left = EmbeddedBuilder::splitDot( right );
                left.length() > 0 && left[ left.length() - 1 ] != '.';
                left += "." + EmbeddedBuilder::splitDot( right ) ) {
                if ( existing.count( left ) > 0 && existing[ left ].type() != Object )
                    return false;
                if ( modForField( left.c_str() ) )
                    return false;
            }
            return true;
        }
        static Mod::Op opFromStr( const char *fn ) {
            const char *valid[] = { "$inc", "$set", "$push" };
            for( int i = 0; i < 3; ++i )
                if ( strcmp( fn, valid[ i ] ) == 0 )
                    return Mod::Op( i );
            uassert( "Invalid modifier specified", false );
            return Mod::INC;
        }
    public:
        void getMods( const BSONObj &from );
        bool applyModsInPlace( const BSONObj &obj ) const;
        BSONObj createNewFromMods( const BSONObj &obj );
        void checkUnindexed( const set<string>& idxKeys ) const {
            for ( vector<Mod>::const_iterator i = mods_.begin(); i != mods_.end(); i++ ) {
                // check if there is an index key that is a parent of mod
                for( const char *dot = strchr( i->fieldName, '.' ); dot; dot = strchr( dot + 1, '.' ) )
                    if ( idxKeys.count( string( i->fieldName, dot - i->fieldName ) ) )
                        uassert("can't $inc/$set an indexed field", false);
                string fullName = i->fieldName;
                // check if there is an index key equal to mod
                if ( idxKeys.count(fullName) )
                    uassert("can't $inc/$set an indexed field", false);
                // check if there is an index key that is a child of mod
                set< string >::const_iterator j = idxKeys.upper_bound( fullName );
                if ( j != idxKeys.end() && j->find( fullName ) == 0 )
                    uassert("can't $inc/$set an indexed field", false);                    
            }
        }
        unsigned size() const { return mods_.size(); }
        bool haveModForField( const char *fieldName ) const {
            // Presumably the number of mods is small, so this loop isn't too expensive.
            for( vector<Mod>::const_iterator i = mods_.begin(); i != mods_.end(); ++i ) {
                if ( strcmp( fieldName, i->fieldName ) == 0 )
                    return true;
            }
            return false;
        }
        bool haveModForFieldOrSubfield( const char *fieldName ) const {
            // Presumably the number of mods is small, so this loop isn't too expensive.
            for( vector<Mod>::const_iterator i = mods_.begin(); i != mods_.end(); ++i ) {
                const char *dot = strchr( i->fieldName, '.' );
                int len = dot ? dot - i->fieldName : strlen( i->fieldName );
                if ( strncmp( fieldName, i->fieldName, len ) == 0 )
                    return true;
            }
            return false;
        }
        const Mod *modForField( const char *fieldName ) const {
            // Presumably the number of mods is small, so this loop isn't too expensive.
            for( vector<Mod>::const_iterator i = mods_.begin(); i != mods_.end(); ++i ) {
                if ( strcmp( fieldName, i->fieldName ) == 0 )
                    return &*i;
            }
            return 0;
        }
        bool havePush() const {
            for ( vector<Mod>::const_iterator i = mods_.begin(); i != mods_.end(); i++ )
                if ( i->op == Mod::PUSH )
                    return true;
            return false;
        }
        void appendSizeSpecForPushes( BSONObjBuilder &b ) const {
            for ( vector<Mod>::const_iterator i = mods_.begin(); i != mods_.end(); i++ ) {
                if ( i->op == Mod::PUSH ) {
                    if ( i->pushStartSize == -1 )
                        b.appendNull( i->fieldName );
                    else
                        b << i->fieldName << BSON( "$size" << i->pushStartSize );
                }
            }
        }
    };
    
    bool ModSet::applyModsInPlace(const BSONObj &obj) const {
        bool inPlacePossible = true;
        // Perform this check first, so that we don't leave a partially modified object
        // on uassert.
        for ( vector<Mod>::const_iterator i = mods_.begin(); i != mods_.end(); ++i ) {
            const Mod& m = *i;
            BSONElement e = obj.getFieldDotted(m.fieldName);
            switch( m.op ) {
                case Mod::INC:
                    uassert( "Cannot apply $inc modifier to non-number", e.isNumber() || e.eoo() );
                    if ( !e.isNumber() )
                        inPlacePossible = false;
                    break;
                case Mod::SET:
                    if ( !( e.isNumber() && m.elt.isNumber() ) &&
                        m.elt.valuesize() != e.valuesize() )
                        inPlacePossible = false;
                    break;
                case Mod::PUSH:
                    uassert( "Cannot apply $push modifier to non-array", e.type() == Array || e.eoo() );
                    inPlacePossible = false;
                    break;
            }
        }
        if ( !inPlacePossible ) {
            return false;
        }
        for ( vector<Mod>::const_iterator i = mods_.begin(); i != mods_.end(); ++i ) {
            const Mod& m = *i;
            BSONElement e = obj.getFieldDotted(m.fieldName);
            if ( m.op == Mod::INC ) {
                m.setn( e.number() + m.getn() );
                BSONElementManipulator( e ).setNumber( m.getn() );
            } else {
                if ( e.isNumber() && m.elt.isNumber() )
                    BSONElementManipulator( e ).setNumber( m.getn() );
                else
                    BSONElementManipulator( e ).replaceTypeAndValue( m.elt );
            }
        }
        return true;
    }

    void ModSet::extractFields( map< string, BSONElement > &fields, const BSONElement &top, const string &base ) {
        if ( top.type() != Object ) {
            fields[ base + top.fieldName() ] = top;
            return;
        }
        BSONObjIterator i( top.embeddedObject() );
        bool empty = true;
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            extractFields( fields, e, base + top.fieldName() + "." );
            empty = false;
        }
        if ( empty )
            fields[ base + top.fieldName() ] = top;            
    }
    
    BSONObj ModSet::createNewFromMods( const BSONObj &obj ) {
        sortMods();
        map< string, BSONElement > existing;
        
        BSONObjBuilder b;
        BSONObjIterator i( obj );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( !haveModForFieldOrSubfield( e.fieldName() ) ) {
                b.append( e );
            } else {
                extractFields( existing, e, "" );
            }
        }
            
        EmbeddedBuilder b2( &b );
        vector< Mod >::iterator m = mods_.begin();
        map< string, BSONElement >::iterator p = existing.begin();
        while( m != mods_.end() || p != existing.end() ) {
            int cmp = compare( m, p, existing.end() );
            if ( cmp <= 0 )
                uassert( "Modifier spec implies existence of an encapsulating object with a name that already represents a non-object,"
                         " or is referenced in another $set clause",
                        mayAddEmbedded( existing, m->fieldName ) );                
            if ( cmp == 0 ) {
                BSONElement e = p->second;
                if ( m->op == Mod::INC ) {
                    m->setn( m->getn() + e.number() );
                    b2.appendAs( m->elt, m->fieldName );
                } else if ( m->op == Mod::SET ) {
                    b2.appendAs( m->elt, m->fieldName );
                } else if ( m->op == Mod::PUSH ) {
                    BSONObjBuilder arr( b2.subarrayStartAs( m->fieldName ) );
                    BSONObjIterator i( e.embeddedObject() );
                    int count = 0;
                    while( i.more() ) {
                        BSONElement arrI = i.next();
                        if ( arrI.eoo() )
                            break;
                        arr.append( arrI );
                        ++count;
                    }
                    stringstream ss;
                    ss << count;
                    string nextIndex = ss.str();
                    arr.appendAs( m->elt, nextIndex.c_str() );
                    arr.done();
                    m->pushStartSize = count;
                }
                ++m;
                ++p;
            } else if ( cmp < 0 ) {
                if ( m->op == Mod::PUSH ) {
                    BSONObjBuilder arr( b2.subarrayStartAs( m->fieldName ) );
                    arr.appendAs( m->elt, "0" );
                    arr.done();
                    m->pushStartSize = -1;
                } else {
                    b2.appendAs( m->elt, m->fieldName );
                }
                ++m;
            } else if ( cmp > 0 ) {
                if ( mayAddEmbedded( existing, p->first ) )
                    b2.appendAs( p->second, p->first ); 
                ++p;
            }
        }
        b2.done();
        return b.obj();
    }
    
    /* get special operations like $inc
       { $inc: { a:1, b:1 } }
       { $set: { a:77 } }
       { $push: { a:55 } }
       NOTE: MODIFIES source from object!
    */
    void ModSet::getMods(const BSONObj &from) {
        BSONObjIterator it(from);
        while ( it.more() ) {
            BSONElement e = it.next();
            if ( e.eoo() )
                break;
            const char *fn = e.fieldName();
            uassert( "Invalid modifier specified", e.type() == Object );
            BSONObj j = e.embeddedObject();
            BSONObjIterator jt(j);
            Mod::Op op = opFromStr( fn );
            if ( op == Mod::INC )
                strcpy((char *) fn, "$set"); // rewrite for op log
            while ( jt.more() ) {
                BSONElement f = jt.next();
                if ( f.eoo() )
                    break;
                Mod m;
                m.op = op;
                m.fieldName = f.fieldName();
                uassert( "Mod on _id not allowed", strcmp( m.fieldName, "_id" ) != 0 );
                uassert( "Invalid mod field name, may not end in a period", m.fieldName[ strlen( m.fieldName ) - 1 ] != '.' );
                for ( vector<Mod>::iterator i = mods_.begin(); i != mods_.end(); i++ ) {
                    uassert( "Field name duplication not allowed with modifiers",
                            strcmp( m.fieldName, i->fieldName ) != 0 );
                }
                uassert( "Modifier $inc allowed for numbers only", f.isNumber() || op != Mod::INC );
                m.elt = f;
                if ( f.type() == NumberDouble ) {
                    m.ndouble = (double *) f.value();
                    m.nint = 0;
                } else if ( f.type() == NumberInt ) {
                    m.ndouble = 0;
                    m.nint = (int *) f.value();
                }
                mods_.push_back( m );
            }
        }
    }

    void checkNoMods( BSONObj o ) {
        BSONObjIterator i( o );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            massert( "Modifiers and non-modifiers cannot be mixed", e.fieldName()[ 0 ] != '$' );
        }
    }
    
    class UpdateOp : public QueryOp {
    public:
        UpdateOp() : nscanned_() {}
        virtual void init() {
            BSONObj pattern = qp().query();
            c_ = qp().newCursor();
            if ( !c_->ok() )
                setComplete();
            else
                matcher_.reset( new KeyValJSMatcher( pattern, qp().indexKey() ) );
        }
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }
            nscanned_++;
            if ( matcher_->matches(c_->currKey(), c_->currLoc()) ) {
                setComplete();
                return;
            }
            c_->advance();
        }
        virtual bool mayRecordPlan() const { return false; }
        virtual QueryOp *clone() const {
            return new UpdateOp();
        }
        auto_ptr< Cursor > c() { return c_; }
        long long nscanned() const { return nscanned_; }
    private:
        auto_ptr< Cursor > c_;
        long long nscanned_;
        auto_ptr< KeyValJSMatcher > matcher_;
    };
    
    int __updateObjects(const char *ns, BSONObj updateobj, BSONObj &pattern, bool upsert, stringstream& ss, bool logop=false) {
        int profile = database->profile;
        
        if ( strstr(ns, ".system.") ) {
            if( strstr(ns, ".system.users") )
                ;
            else {
                out() << "\nERROR: attempt to update in system namespace " << ns << endl;
                ss << " can't update system namespace ";
                return 0;
            }
        }
        
        QueryPlanSet qps( ns, pattern, BSONObj() );
        UpdateOp original;
        shared_ptr< UpdateOp > u = qps.runOp( original );
        massert( u->exceptionMessage(), u->complete() );
        auto_ptr< Cursor > c = u->c();
        if ( c->ok() ) {
            Record *r = c->_current();
            BSONObj js(r);
            
            if ( logop ) {
                BSONObjBuilder idPattern;
                BSONElement id;
                // NOTE: If the matching object lacks an id, we'll log
                // with the original pattern.  This isn't replay-safe.
                // It might make sense to suppress the log instead
                // if there's no id.
                if ( js.getObjectID( id ) ) {
                    idPattern.append( id );
                    pattern = idPattern.obj();
                }
            }
            
            /* note: we only update one row and quit.  if you do multiple later,
             be careful or multikeys in arrays could break things badly.  best
             to only allow updating a single row with a multikey lookup.
             */
            
            if ( profile )
                ss << " nscanned:" << u->nscanned();
            
            /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
             regular ones at the moment. */
            const char *firstField = updateobj.firstElement().fieldName();
            if ( firstField[0] == '$' ) {
                ModSet mods;
                mods.getMods(updateobj);
                NamespaceDetailsTransient& ndt = NamespaceDetailsTransient::get(ns);
                set<string>& idxKeys = ndt.indexKeys();
                mods.checkUnindexed( idxKeys );
                if ( mods.applyModsInPlace( c->currLoc().obj() ) ) {
                    if ( profile )
                        ss << " fastmod ";
                } else {
                    BSONObj newObj = mods.createNewFromMods( c->currLoc().obj() );
                    theDataFileMgr.update(ns, r, c->currLoc(), newObj.objdata(), newObj.objsize(), ss);                    
                }
                if ( logop ) {
                    if ( mods.size() ) {
                        if ( mods.havePush() ) {
                            BSONObjBuilder patternBuilder;
                            patternBuilder.appendElements( pattern );
                            mods.appendSizeSpecForPushes( patternBuilder );
                            pattern = patternBuilder.obj();                        
                        }
                        logOp("u", ns, updateobj, &pattern );
                        return 5;
                    }
                }
                return 2;
            } else {
                BSONElementManipulator::lookForTimestamps( updateobj );
                checkNoMods( updateobj );
            }
            
            theDataFileMgr.update(ns, r, c->currLoc(), updateobj.objdata(), updateobj.objsize(), ss);
            return 1;
        }
        
        if ( profile )
            ss << " nscanned:" << u->nscanned();
        
        if ( upsert ) {
            if ( updateobj.firstElement().fieldName()[0] == '$' ) {
                /* upsert of an $inc. build a default */
                ModSet mods;
                mods.getMods(updateobj);
                BSONObj newObj = pattern.copy();
                if ( mods.applyModsInPlace( newObj ) ) {
                    //
                } else {
                    newObj = mods.createNewFromMods( newObj );
                }
                if ( profile )
                    ss << " fastmodinsert ";
                theDataFileMgr.insert(ns, newObj);
                if ( profile )
                    ss << " fastmodinsert ";
                if ( logop )
                    logOp( "i", ns, newObj );
                return 3;
            }
            if ( profile )
                ss << " upsert ";
            theDataFileMgr.insert(ns, updateobj);
            if ( logop )
                logOp( "i", ns, updateobj );
            return 4;
        }
        return 0;
    }
    
    /* todo:
     _ smart requery find record immediately
     (clean return codes up later...)
     */
    int _updateObjects(const char *ns, BSONObj updateobj, BSONObj pattern, bool upsert, stringstream& ss, bool logop=false) {
        return __updateObjects( ns, updateobj, pattern, upsert, ss, logop );
    }
        
    bool updateObjects(const char *ns, BSONObj updateobj, BSONObj pattern, bool upsert, stringstream& ss) {
        int rc = __updateObjects(ns, updateobj, pattern, upsert, ss, true);
        if ( rc != 5 && rc != 0 && rc != 4 && rc != 3 )
            logOp("u", ns, updateobj, &pattern, &upsert);
        return ( rc == 1 || rc == 2 || rc == 5 );
    }

    int queryTraceLevel = 0;
    int otherTraceLevel = 0;

    int initialExtentSize(int len);

    bool runCommands(const char *ns, BSONObj& jsobj, stringstream& ss, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        try {
            return _runCommands(ns, jsobj, ss, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch ( AssertionException& e ) {
            if ( !e.msg.empty() )
                anObjBuilder.append("assertion", e.msg);
        }
        ss << " assertion ";
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.append((void*) x.objdata(), x.objsize());
        return true;
    }

    int nCaught = 0;

    void killCursors(int n, long long *ids) {
        int k = 0;
        for ( int i = 0; i < n; i++ ) {
            if ( ClientCursor::erase(ids[i]) )
                k++;
        }
        log() << "killCursors: found " << k << " of " << n << '\n';
    }

    BSONObj id_obj = fromjson("{\"_id\":ObjectId( \"000000000000000000000000\" )}");
    BSONObj empty_obj = fromjson("{}");

    /* This is for languages whose "objects" are not well ordered (JSON is well ordered).
       [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
    */
    inline BSONObj transformOrderFromArrayFormat(BSONObj order) {
        /* note: this is slow, but that is ok as order will have very few pieces */
        BSONObjBuilder b;
        char p[2] = "0";

        while ( 1 ) {
            BSONObj j = order.getObjectField(p);
            if ( j.isEmpty() )
                break;
            BSONElement e = j.firstElement();
            uassert("bad order array", !e.eoo());
            uassert("bad order array [2]", e.isNumber());
            b.append(e);
            (*p)++;
            uassert("too many ordering elements", *p <= '9');
        }

        return b.obj();
    }


//int dump = 0;

    /* empty result for error conditions */
    QueryResult* emptyMoreResult(long long cursorid) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        QueryResult *qr = (QueryResult *) b.buf();
        qr->cursorId = 0; // 0 indicates no more data to retrieve.
        qr->startingFrom = 0;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->nReturned = 0;
        b.decouple();
        return qr;
    }

    QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid) {
        BufBuilder b(32768);

        ClientCursor *cc = ClientCursor::find(cursorid);

        b.skip(sizeof(QueryResult));

        int resultFlags = 0;
        int start = 0;
        int n = 0;

        if ( !cc ) {
            log() << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = QueryResult::ResultFlag_CursorNotFound;
        }
        else {
            start = cc->pos;
            Cursor *c = cc->c.get();
            c->checkLocation();
            while ( 1 ) {
                if ( !c->ok() ) {
                    if ( c->tailable() ) {
                        if ( c->advance() ) {
                            continue;
                        }
                        break;
                    }
                    bool ok = ClientCursor::erase(cursorid);
                    assert(ok);
                    cursorid = 0;
                    cc = 0;
                    break;
                }
                bool deep;
                if ( !cc->matcher->matches(c->currKey(), c->currLoc(), &deep) ) {
                }
                else {
                    //out() << "matches " << c->currLoc().toString() << ' ' << deep << '\n';
                    if ( deep && c->getsetdup(c->currLoc()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        BSONObj js = c->current();
                        bool ok = fillQueryResultFromObj(b, cc->filter.get(), js);
                        if ( ok ) {
                            n++;
                            if ( (ntoreturn>0 && (n >= ntoreturn || b.len() > MaxBytesToReturnToClientAtOnce)) ||
                                    (ntoreturn==0 && b.len()>1*1024*1024) ) {
                                c->advance();
                                cc->pos += n;
                                //cc->updateLocation();
                                break;
                            }
                        }
                    }
                }
                c->advance();
            }
            if ( cc )
                cc->updateLocation();
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->resultFlags() = resultFlags;
        qr->cursorId = cursorid;
        qr->startingFrom = start;
        qr->nReturned = n;
        b.decouple();

        return qr;
    }

    class CountOp : public QueryOp {
    public:
        CountOp( const BSONObj &spec ) : spec_( spec ), count_(), bc_() {}
        virtual void init() {
            query_ = spec_.getObjectField( "query" );
            spec_.getObjectField( "fields" ).getFieldNames( fields_ );
            c_ = qp().newCursor();
            if ( qp().exactKeyMatch() && fields_.empty() ) {
                bc_ = dynamic_cast< BtreeCursor* >( c_.get() );
                bc_->forgetEndKey();
            }
            else {
                matcher_.reset( new KeyValJSMatcher( query_, c_->indexKeyPattern() ) );
            }
        }
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }
            if ( bc_ ) {
                if ( firstMatch_.isEmpty() ) {
                    firstMatch_ = bc_->currKeyNode().key;
                    // if not match
                    if ( query_.woCompare( firstMatch_, BSONObj(), false ) ) {
                        setComplete();
                        return;
                    }
                    ++count_;
                } else {
                    if ( !firstMatch_.woEqual( bc_->currKeyNode().key ) ) {
                        setComplete();
                        return;
                    }
                    ++count_;
                }
            } else {
                bool deep;
                if ( !matcher_->matches(c_->currKey(), c_->currLoc(), &deep) ) {
                }
                else if ( !deep || !c_->getsetdup(c_->currLoc()) ) { // i.e., check for dups on deep items only
                    bool match = true;
                    if ( !fields_.empty() ) {
                        BSONObj js = c_->current();
                        for( set< string >::iterator i = fields_.begin(); i != fields_.end(); ++i ) {
                            if ( js.getFieldDotted( i->c_str() ).eoo() ) {
                                match = false;
                                break;
                            }
                        }
                    }
                    if ( match )
                        ++count_;
                }                
            }
            c_->advance();
        }
        virtual QueryOp *clone() const {
            return new CountOp( spec_ );
        }
        long long count() const { return count_; }
        virtual bool mayRecordPlan() const { return true; }
    private:
        BSONObj spec_;
        long long count_;
        auto_ptr< Cursor > c_;
        BSONObj query_;
        set< string > fields_;
        BtreeCursor *bc_;
        auto_ptr< KeyValJSMatcher > matcher_;
        BSONObj firstMatch_;
    };
    
    /* { count: "collectionname"[, query: <query>] }
         returns -1 on ns does not exist error.
    */    
    long long runCount( const char *ns, const BSONObj &cmd, string &err ) {
        NamespaceDetails *d = nsdetails( ns );
        if ( !d ) {
            err = "ns missing";
            return -1;
        }
        BSONObj query = cmd.getObjectField("query");
        BSONObj fields = cmd.getObjectField("fields");
        // count of all objects
        if ( query.isEmpty() && fields.isEmpty() ) {
            return d->nrecords;
        }
        QueryPlanSet qps( ns, query, BSONObj() );
        CountOp original( cmd );
        shared_ptr< CountOp > res = qps.runOp( original );
        if ( !res->complete() ) {
            log() << "Count with ns: " << ns << " and query: " << query
                  << " failed with exception: " << res->exceptionMessage()
                  << endl;
            return 0;
        }
        return res->count();
    }
        
    class DoQueryOp : public QueryOp {
    public:
        DoQueryOp( int ntoskip, int ntoreturn, const BSONObj &order, bool wantMore,
                  bool explain, set< string > *filter, int queryOptions ) :
        b_( 32768 ),
        ntoskip_( ntoskip ),
        ntoreturn_( ntoreturn ),
        order_( order ),
        wantMore_( wantMore ),
        explain_( explain ),
        filter_( filter ),
        ordering_(),
        nscanned_(),
        queryOptions_( queryOptions ),
        n_(),
        soSize_(),
        saveClientCursor_(),
        findingStart_( (queryOptions & Option_OplogReplay) != 0 ) 
        {}

        virtual void init() {
            b_.skip( sizeof( QueryResult ) );
            
            if ( findingStart_ )
                c_ = qp().newReverseCursor();
            else
                c_ = qp().newCursor();
            
            matcher_.reset(new KeyValJSMatcher(qp().query(), qp().indexKey()));
            
            if ( qp().scanAndOrderRequired() ) {
                ordering_ = true;
                so_.reset( new ScanAndOrder( ntoskip_, ntoreturn_, order_ ) );
                wantMore_ = false;
            }
        }
        virtual void next() {
            if ( findingStart_ ) {
                if ( !c_->ok() ) {
                    findingStart_ = false;
                    c_ = qp().newCursor();
                } else if ( !matcher_->matches( c_->currKey(), c_->currLoc() ) ) {
                    findingStart_ = false;
                    c_ = qp().newCursor( c_->currLoc() );
                } else {
                    c_->advance();
                    return;
                }
            }
            
            if ( !c_->ok() ) {
                finish();
                return;
            }
            
            nscanned_++;
            bool deep;
            if ( !matcher_->matches(c_->currKey(), c_->currLoc(), &deep) ) {
            }
            else if ( !deep || !c_->getsetdup(c_->currLoc()) ) { // i.e., check for dups on deep items only
                BSONObj js = c_->current();
                // got a match.
                assert( js.objsize() >= 0 ); //defensive for segfaults
                if ( ordering_ ) {
                    // note: no cursors for non-indexed, ordered results.  results must be fairly small.
                    so_->add(js);
                }
                else if ( ntoskip_ > 0 ) {
                    ntoskip_--;
                } else {
                    if ( explain_ ) {
                        n_++;
                        if ( n_ >= ntoreturn_ && !wantMore_ ) {
                            // .limit() was used, show just that much.
                            finish();
                            return;
                        }
                    }
                    else {
                        bool ok = fillQueryResultFromObj(b_, filter_, js);
                        if ( ok ) n_++;
                        if ( ok ) {
                            if ( (ntoreturn_>0 && (n_ >= ntoreturn_ || b_.len() > MaxBytesToReturnToClientAtOnce)) ||
                                (ntoreturn_==0 && (b_.len()>1*1024*1024 || n_>=101)) ) {
                                /* if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there
                                 is only a size limit.  The idea is that on a find() where one doesn't use much results,
                                 we don't return much, but once getmore kicks in, we start pushing significant quantities.
                                 
                                 The n limit (vs. size) is important when someone fetches only one small field from big
                                 objects, which causes massive scanning server-side.
                                 */
                                /* if only 1 requested, no cursor saved for efficiency...we assume it is findOne() */
                                if ( wantMore_ && ntoreturn_ != 1 ) {
                                    if ( useCursors ) {
                                        c_->advance();
                                        if ( c_->ok() ) {
                                            // more...so save a cursor
                                            saveClientCursor_ = true;
                                        }
                                    }
                                }
                                finish();
                                return;
                            }
                        }
                    }
                }
            }
            c_->advance();            
        }
        void finish() {
            if ( explain_ ) {
                n_ = ordering_ ? so_->size() : n_;
            } else if ( ordering_ ) {
                so_->fill(b_, filter_, n_);
            }
            if ( ( queryOptions_ & Option_CursorTailable ) && ntoreturn_ != 1 ) {
                c_->setTailable();
            }
            // If the tailing request succeeded.
            if ( c_->tailable() ) {
                saveClientCursor_ = true;
            }
            setComplete();            
        }
        virtual bool mayRecordPlan() const { return ntoreturn_ != 1; }
        virtual QueryOp *clone() const {
            return new DoQueryOp( ntoskip_, ntoreturn_, order_, wantMore_, explain_, filter_, queryOptions_ );
        }
        BufBuilder &builder() { return b_; }
        bool scanAndOrderRequired() const { return ordering_; }
        auto_ptr< Cursor > cursor() { return c_; }
        auto_ptr< KeyValJSMatcher > matcher() { return matcher_; }
        int n() const { return n_; }
        long long nscanned() const { return nscanned_; }
        bool saveClientCursor() const { return saveClientCursor_; }
    private:
        BufBuilder b_;
        int ntoskip_;
        int ntoreturn_;
        BSONObj order_;
        bool wantMore_;
        bool explain_;
        set< string > *filter_;   
        bool ordering_;
        auto_ptr< Cursor > c_;
        long long nscanned_;
        int queryOptions_;
        auto_ptr< KeyValJSMatcher > matcher_;
        int n_;
        int soSize_;
        bool saveClientCursor_;
        auto_ptr< ScanAndOrder > so_;
        bool findingStart_;
    };
    
    auto_ptr< QueryResult > runQuery(Message& m, stringstream& ss ) {
        DbMessage d( m );
        QueryMessage q( d );
        const char *ns = q.ns;
        int ntoskip = q.ntoskip;
        int _ntoreturn = q.ntoreturn;
        BSONObj jsobj = q.query;
        auto_ptr< set< string > > filter = q.fields;
        int queryOptions = q.queryOptions;
        
        Timer t;
        log(2) << "runQuery: " << ns << jsobj << endl;
        
        long long nscanned = 0;
        bool wantMore = true;
        int ntoreturn = _ntoreturn;
        if ( _ntoreturn < 0 ) {
            /* _ntoreturn greater than zero is simply a hint on how many objects to send back per 
             "cursor batch".
             A negative number indicates a hard limit.
             */
            ntoreturn = -_ntoreturn;
            wantMore = false;
        }
        ss << "query " << ns << " ntoreturn:" << ntoreturn;
        {
            string s = jsobj.toString();
            strncpy(currentOp.query, s.c_str(), sizeof(currentOp.query)-1);
        }
        
        BufBuilder bb;
        BSONObjBuilder cmdResBuf;
        long long cursorid = 0;
        
        bb.skip(sizeof(QueryResult));
        
        auto_ptr< QueryResult > qr;
        int n = 0;
        
        /* we assume you are using findOne() for running a cmd... */
        if ( ntoreturn == 1 && runCommands(ns, jsobj, ss, bb, cmdResBuf, false, queryOptions) ) {
            n = 1;
            qr.reset( (QueryResult *) bb.buf() );
            bb.decouple();
            qr->resultFlags() = 0;
            qr->len = bb.len();
            ss << " reslen:" << bb.len();
            //	qr->channel = 0;
            qr->setOperation(opReply);
            qr->cursorId = cursorid;
            qr->startingFrom = 0;
            qr->nReturned = n;            
        }
        else {
            
            AuthenticationInfo *ai = authInfo.get();
            uassert("unauthorized", ai->isAuthorized(database->name.c_str()));
            
            uassert( "not master", isMaster() || (queryOptions & Option_SlaveOk) );

            BSONElement hint;
            bool explain = false;
            bool _gotquery = false;
            BSONObj query;// = jsobj.getObjectField("query");
            {
                BSONElement e = jsobj.findElement("query");
                if ( !e.eoo() && (e.type() == Object || e.type() == Array) ) {
                    query = e.embeddedObject();
                    _gotquery = true;
                }
            }
            BSONObj order;
            {
                BSONElement e = jsobj.findElement("orderby");
                if ( !e.eoo() ) {
                    order = e.embeddedObjectUserCheck();
                    if ( e.type() == Array )
                        order = transformOrderFromArrayFormat(order);
                }
            }
            if ( !_gotquery && order.isEmpty() )
                query = jsobj;
            else {
                explain = jsobj.getBoolField("$explain");
                if ( useHints )
                    hint = jsobj.getField("$hint");
            }
            
            /* The ElemIter will not be happy if this isn't really an object. So throw exception
             here when that is true.
             (Which may indicate bad data from appserver?)
             */
            if ( query.objsize() == 0 ) {
                out() << "Bad query object?\n  jsobj:";
                out() << jsobj.toString() << "\n  query:";
                out() << query.toString() << endl;
                uassert("bad query object", false);
            }

            BSONObj oldPlan;
            if ( explain && hint.eoo() ) {
                QueryPlanSet qps( ns, query, order );
                if ( qps.usingPrerecordedPlan() )
                    oldPlan = qps.explain();
            }
            QueryPlanSet qps( ns, query, order, &hint, !explain );
            DoQueryOp original( ntoskip, ntoreturn, order, wantMore, explain, filter.get(), queryOptions );
            shared_ptr< DoQueryOp > o = qps.runOp( original );
            DoQueryOp &dqo = *o;
            massert( dqo.exceptionMessage(), dqo.complete() );
            n = dqo.n();
            nscanned = dqo.nscanned();
            if ( dqo.scanAndOrderRequired() )
                ss << " scanAndOrder ";
            auto_ptr< Cursor > c = dqo.cursor();
            if ( dqo.saveClientCursor() ) {
                ClientCursor *cc = new ClientCursor();
                cc->c = c;
                cursorid = cc->cursorid;
                DEV out() << "  query has more, cursorid: " << cursorid << endl;
                cc->matcher = dqo.matcher();
                cc->ns = ns;
                cc->pos = n;
                cc->filter = filter;
                cc->originalMessage = m;
                cc->updateLocation();
                if ( !cc->c->ok() && cc->c->tailable() ) {
                    DEV out() << "  query has no more but tailable, cursorid: " << cursorid << endl;
                } else {
                    DEV out() << "  query has more, cursorid: " << cursorid << endl;
                }
            }
            if ( explain ) {
                BSONObjBuilder builder;
                builder.append("cursor", c->toString());
                builder.append("startKey", c->prettyStartKey());
                builder.append("endKey", c->prettyEndKey());
                builder.append("nscanned", double( dqo.nscanned() ) );
                builder.append("n", n);
                if ( dqo.scanAndOrderRequired() )
                    builder.append("scanAndOrder", true);
                builder.append("millis", t.millis());
                if ( !oldPlan.isEmpty() )
                    builder.append( "oldPlan", oldPlan.firstElement().embeddedObject().firstElement().embeddedObject() );
                if ( hint.eoo() )
                    builder.appendElements(qps.explain());
                BSONObj obj = builder.done();
                fillQueryResultFromObj(dqo.builder(), 0, obj);
                n = 1;
            }
            qr.reset( (QueryResult *) dqo.builder().buf() );
            dqo.builder().decouple();
            qr->cursorId = cursorid;
            qr->resultFlags() = 0;
            qr->len = dqo.builder().len();
            ss << " reslen:" << qr->len;
            qr->setOperation(opReply);
            qr->startingFrom = 0;
            qr->nReturned = n;
        }
        
        int duration = t.millis();
        if ( (database && database->profile) || duration >= 100 ) {
            ss << " nscanned:" << nscanned << ' ';
            if ( ntoskip )
                ss << " ntoskip:" << ntoskip;
            if ( database && database->profile )
                ss << " \nquery: ";
            ss << jsobj << ' ';
        }
        ss << " nreturned:" << n;
        return qr;        
    }    
    
} // namespace mongo
