// update.cpp

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
#include "queryoptimizer.h"
#include "repl.h"

namespace mongo {

    // utility class for assembling hierarchical objects
    class EmbeddedBuilder {
    public:
        EmbeddedBuilder( BSONObjBuilder *b ) {
            _builders.push_back( make_pair( "", b ) );
        }
        // It is assumed that the calls to prepareContext will be made with the 'name'
        // parameter in lex ascending order.
        void prepareContext( string &name ) {
            int i = 1, n = _builders.size();
            while( i < n && 
                   name.substr( 0, _builders[ i ].first.length() ) == _builders[ i ].first && 
                   ( name[ _builders[i].first.length() ] == '.' || name[ _builders[i].first.length() ] == 0 )
                   ){
                name = name.substr( _builders[ i ].first.length() + 1 );
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
            while( ! _builderStorage.empty() )
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
            _builders.push_back( make_pair( name, newBuilder.get() ) );
            _builderStorage.push_back( newBuilder );
        }
        void popBuilder() {
            back()->done();
            _builders.pop_back();
            _builderStorage.pop_back();
        }

        BSONObjBuilder *back() { return _builders.back().second; }
        
        vector< pair< string, BSONObjBuilder * > > _builders;
        vector< shared_ptr< BSONObjBuilder > > _builderStorage;

    };
    
    /* Used for modifiers such as $inc, $set, ... */
    struct Mod {
        enum Op { INC, SET, PUSH, PUSH_ALL, PULL, PULL_ALL , POP } op;
        const char *fieldName;

        // kind of lame; fix one day?
        double *ndouble;
        int *nint;
        long long *nlong;

        BSONElement elt;
        int pushStartSize;

        /* [dm] why is this const? (or rather, why was setn const?)  i see why but think maybe clearer if were not.  */
        void inc(BSONElement& n) const { 
            uassert( "$inc value is not a number", n.isNumber() );
            if( ndouble ) 
                *ndouble += n.numberDouble();
            else if( nint )
                *nint += n.numberInt();
            else
                *nlong += n.numberLong();\
        }

        void setElementToOurNumericValue(BSONElement& e) const { 
            BSONElementManipulator manip(e);
            if( e.type() == NumberLong )
                manip.setLong(_getlong());
            else
                manip.setNumber(_getn());
        }

        double _getn() const {
            if( ndouble ) return *ndouble;
            if( nint ) return *nint;
            return (double) *nlong;
        }
        long long _getlong() const {
            if( nlong ) return *nlong; 
            if( ndouble ) return (long long) *ndouble;
            return *nint;
        }
        bool operator<( const Mod &other ) const {
            return strcmp( fieldName, other.fieldName ) < 0;
        }
        
        bool arrayDep() const {
            switch (op){
            case PUSH:
            case PUSH_ALL:
            case POP:
                return true;
            default:
                return false;
            }
        }
        
    };

    class ModSet {
        vector< Mod > _mods;
        void sortMods() {
            sort( _mods.begin(), _mods.end() );
        }
        
        static void extractFields( map< string, BSONElement > &fields, const BSONElement &top, const string &base );
        
        FieldCompareResult compare( const vector< Mod >::iterator &m, map< string, BSONElement >::iterator &p, const map< string, BSONElement >::iterator &pEnd ) const {
            bool mDone = ( m == _mods.end() );
            bool pDone = ( p == pEnd );
            assert( ! mDone );
            assert( ! pDone );
            if ( mDone && pDone )
                return SAME;
            // If one iterator is done we want to read from the other one, so say the other one is lower.
            if ( mDone )
                return RIGHT_BEFORE;
            if ( pDone )
                return LEFT_BEFORE;

            return compareDottedFieldNames( m->fieldName, p->first.c_str() );
        }
        
        void appendNewFromMod( Mod& m , EmbeddedBuilder& b ){
            if ( m.op == Mod::PUSH ) {
                BSONObjBuilder arr( b.subarrayStartAs( m.fieldName ) );
                arr.appendAs( m.elt, "0" );
                arr.done();
                m.pushStartSize = -1;
            } 
            else if ( m.op == Mod::PUSH_ALL ) {
                b.appendAs( m.elt, m.fieldName );
                m.pushStartSize = -1;
            } 
            else if ( m.op != Mod::PULL && m.op != Mod::PULL_ALL ) {
                b.appendAs( m.elt, m.fieldName );
            }
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
            const char *valid[] = { "$inc", "$set", "$push", "$pushAll", "$pull", "$pullAll" , "$pop" };
            for( int i = 0; i < 7; ++i )
                if ( strcmp( fn, valid[ i ] ) == 0 )
                    return Mod::Op( i );
            uassert( "Invalid modifier specified " + string( fn ), false );
            return Mod::INC;
        }
    public:
        void getMods( const BSONObj &from );
        bool applyModsInPlace( const BSONObj &obj ) const;
        BSONObj createNewFromMods( const BSONObj &obj );

        void checkUnindexed( const set<string>& idxKeys ) const {
            for ( vector<Mod>::const_iterator i = _mods.begin(); i != _mods.end(); i++ ) {
                // check if there is an index key that is a parent of mod
                for( const char *dot = strchr( i->fieldName, '.' ); dot; dot = strchr( dot + 1, '.' ) )
                    if ( idxKeys.count( string( i->fieldName, dot - i->fieldName ) ) )
                        uassert("E12010 can't $inc/$set an indexed field ", false);
                string fullName = i->fieldName;
                // check if there is an index key equal to mod
                if ( idxKeys.count(fullName) )
                    uassert("E12011 can't $inc/$set an indexed field", false);
                // check if there is an index key that is a child of mod
                set< string >::const_iterator j = idxKeys.upper_bound( fullName );
                if ( j != idxKeys.end() && j->find( fullName ) == 0 && (*j)[fullName.size()] == '.' ){
                    uassert("E12012 can't $inc/$set an indexed field", false);                    
                }
            }
        }

        unsigned size() const { return _mods.size(); }
        bool haveModForField( const char *fieldName ) const {
            // Presumably the number of mods is small, so this loop isn't too expensive.
            for( vector<Mod>::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
                if ( strlen( fieldName ) == strlen( i->fieldName ) && strcmp( fieldName, i->fieldName ) == 0 )
                    return true;
            }
            return false;
        }
        bool haveModForFieldOrSubfield( const char *fieldName ) const {
            // Presumably the number of mods is small, so this loop isn't too expensive.
            for( vector<Mod>::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
                const char *dot = strchr( i->fieldName, '.' );
                size_t len = dot ? dot - i->fieldName : strlen( i->fieldName );
                if ( len == strlen( fieldName ) && strncmp( fieldName, i->fieldName, len ) == 0 )
                    return true;
            }
            return false;
        }
        const Mod *modForField( const char *fieldName ) const {
            // Presumably the number of mods is small, so this loop isn't too expensive.
            for( vector<Mod>::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
                if ( strcmp( fieldName, i->fieldName ) == 0 )
                    return &*i;
            }
            return 0;
        }
        bool haveArrayDepMod() const {
            for ( vector<Mod>::const_iterator i = _mods.begin(); i != _mods.end(); i++ )
                if ( i->arrayDep() )
                    return true;
            return false;
        }
        void appendSizeSpecForArrayDepMods( BSONObjBuilder &b ) const {
            for ( vector<Mod>::const_iterator i = _mods.begin(); i != _mods.end(); i++ ) {
                if ( i->arrayDep() ){
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
        for ( vector<Mod>::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            const Mod& m = *i;
            BSONElement e = obj.getFieldDotted(m.fieldName);
            if ( e.eoo() ) {
                inPlacePossible = false;
            } else {
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
                case Mod::PUSH_ALL:
                    uassert( "Cannot apply $push/$pushAll modifier to non-array", e.type() == Array || e.eoo() );
                    inPlacePossible = false;
                    break;
                case Mod::PULL:
                case Mod::PULL_ALL: {
                    uassert( "Cannot apply $pull/$pullAll modifier to non-array", e.type() == Array || e.eoo() );
                    BSONObjIterator i( e.embeddedObject() );
                    while( inPlacePossible && i.moreWithEOO() ) {
                        BSONElement arrI = i.next();
                        if ( arrI.eoo() )
                            break;
                        if ( m.op == Mod::PULL ) {
                            if ( arrI.woCompare( m.elt, false ) == 0 ) {
                                inPlacePossible = false;
                            }
                        } else if ( m.op == Mod::PULL_ALL ) {
                            BSONObjIterator j( m.elt.embeddedObject() );
                            while( inPlacePossible && j.moreWithEOO() ) {
                                BSONElement arrJ = j.next();
                                if ( arrJ.eoo() )
                                    break;
                                if ( arrI.woCompare( arrJ, false ) == 0 ) {
                                    inPlacePossible = false;
                                }
                            }
                        }
                    }
                    break;
                }
                case Mod::POP: {
                    uassert( "Cannot apply $pop modifier to non-array", e.type() == Array || e.eoo() );
                    if ( ! e.embeddedObject().isEmpty() )
                        inPlacePossible = false;
                    break;
                }
                }
            }
        }
        if ( !inPlacePossible ) {
            return false;
        }
        for ( vector<Mod>::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            const Mod& m = *i;
            BSONElement e = obj.getFieldDotted(m.fieldName);
            if ( m.op == Mod::PULL || m.op == Mod::PULL_ALL )
                continue;

            // [dm] the BSONElementManipulator statements below are for replication (correct?)
            if ( m.op == Mod::INC ) {
                m.inc(e);
                m.setElementToOurNumericValue(e);
            } else {
                if ( e.isNumber() && m.elt.isNumber() ) {
                    // todo: handle NumberLong:
                    m.setElementToOurNumericValue(e);
                } else
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
        while( i.moreWithEOO() ) {
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
        while( i.moreWithEOO() ) {
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
        vector< Mod >::iterator m = _mods.begin();
        map< string, BSONElement >::iterator p = existing.begin();
        while( m != _mods.end() || p != existing.end() ) {

            if ( m == _mods.end() ){
                // no more mods, just regular elements
                assert( p != existing.end() );
                if ( mayAddEmbedded( existing, p->first ) )
                    b2.appendAs( p->second, p->first ); 
                p++;
                continue;
            }
            
            if ( p == existing.end() ){
                uassert( "Modifier spec implies existence of an encapsulating object with a name that already represents a non-object,"
                         " or is referenced in another $set clause",
                         mayAddEmbedded( existing, m->fieldName ) );                
                // $ modifier applied to missing field -- create field from scratch
                appendNewFromMod( *m , b2 );
                m++;
                continue;
            }

            FieldCompareResult cmp = compareDottedFieldNames( m->fieldName , p->first );
            if ( cmp <= 0 )
                uassert( "Modifier spec implies existence of an encapsulating object with a name that already represents a non-object,"
                         " or is referenced in another $set clause",
                         mayAddEmbedded( existing, m->fieldName ) );                
            if ( cmp == 0 ) {
                BSONElement e = p->second;
                if ( m->op == Mod::INC ) {
                    m->inc(e);
                    //m->setn( m->getn() + e.number() );
                    b2.appendAs( m->elt, m->fieldName );
                } else if ( m->op == Mod::SET ) {
                    b2.appendAs( m->elt, m->fieldName );
                } else if ( m->op == Mod::PUSH || m->op == Mod::PUSH_ALL ) {
                    BSONObjBuilder arr( b2.subarrayStartAs( m->fieldName ) );
                    BSONObjIterator i( e.embeddedObject() );
                    int startCount = 0;
                    while( i.moreWithEOO() ) {
                        BSONElement arrI = i.next();
                        if ( arrI.eoo() )
                            break;
                        arr.append( arrI );
                        ++startCount;
                    }
                    if ( m->op == Mod::PUSH ) {
                        stringstream ss;
                        ss << startCount;
                        string nextIndex = ss.str();
                        arr.appendAs( m->elt, nextIndex.c_str() );
                    } else {
                        BSONObjIterator i( m->elt.embeddedObject() );
                        int count = startCount;
                        while( i.moreWithEOO() ) {
                            BSONElement arrI = i.next();
                            if ( arrI.eoo() )
                                break;
                            stringstream ss;
                            ss << count++;
                            string nextIndex = ss.str();
                            arr.appendAs( arrI, nextIndex.c_str() );
                        }
                    }
                    arr.done();
                    m->pushStartSize = startCount;
                } else if ( m->op == Mod::PULL || m->op == Mod::PULL_ALL ) {
                    BSONObjBuilder arr( b2.subarrayStartAs( m->fieldName ) );
                    BSONObjIterator i( e.embeddedObject() );
                    int count = 0;
                    while( i.moreWithEOO() ) {
                        BSONElement arrI = i.next();
                        if ( arrI.eoo() )
                            break;
                        bool allowed = true;
                        if ( m->op == Mod::PULL ) {
                            allowed = ( arrI.woCompare( m->elt, false ) != 0 );
                        } else {
                            BSONObjIterator j( m->elt.embeddedObject() );
                            while( allowed && j.moreWithEOO() ) {
                                BSONElement arrJ = j.next();
                                if ( arrJ.eoo() )
                                    break;
                                allowed = ( arrI.woCompare( arrJ, false ) != 0 );
                            }
                        }
                        if ( allowed ) {
                            stringstream ss;
                            ss << count++;
                            string index = ss.str();
                            arr.appendAs( arrI, index.c_str() );
                        }
                    }
                    arr.done();
                }
                else if ( m->op == Mod::POP ){
                    int startCount = 0;
                    BSONObjBuilder arr( b2.subarrayStartAs( m->fieldName ) );
                    BSONObjIterator i( e.embeddedObject() );
                    if ( m->elt.isNumber() && m->elt.number() < 0 ){
                        if ( i.more() ) i.next();
                        int count = 0;
                        startCount++;
                        while( i.more() ) {
                            arr.appendAs( i.next() , arr.numStr( count++ ).c_str() );
                            startCount++;
                        }
                    }
                    else {
                        while( i.more() ) {
                            startCount++;
                            BSONElement arrI = i.next();
                            if ( i.more() ){
                                arr.append( arrI );
                            }
                        }
                    }
                    arr.done();
                    m->pushStartSize = startCount;
                }
                ++m;
                ++p;
            } 
            else if ( cmp < 0 ) {
                // $ modifier applied to missing field -- create field from scratch
                appendNewFromMod( *m , b2 );
                m++;
            } 
            else if ( cmp > 0 ) {
                // No $ modifier
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
       { $pushAll: { a:[77,88] } }
       { $pull: { a:66 } }
       { $pullAll : { a:[99,1010] } }
       NOTE: MODIFIES source from object!
    */
    void ModSet::getMods(const BSONObj &from) {
        BSONObjIterator it(from);
        while ( it.moreWithEOO() ) {
            BSONElement e = it.next();
            if ( e.eoo() )
                break;
            const char *fn = e.fieldName();
            uassert( "Invalid modifier specified" + string( fn ), e.type() == Object );
            BSONObj j = e.embeddedObject();
            BSONObjIterator jt(j);
            Mod::Op op = opFromStr( fn );
            if ( op == Mod::INC )
                strcpy((char *) fn, "$set"); // rewrite for op log
            while ( jt.more() ) {
                BSONElement f = jt.next();
                Mod m;
                m.op = op;
                m.fieldName = f.fieldName();
                uassert( "Mod on _id not allowed", strcmp( m.fieldName, "_id" ) != 0 );
                uassert( "Invalid mod field name, may not end in a period", m.fieldName[ strlen( m.fieldName ) - 1 ] != '.' );
                for ( vector<Mod>::iterator i = _mods.begin(); i != _mods.end(); i++ ) {
                    uassert( "Field name duplication not allowed with modifiers",
                             strcmp( m.fieldName, i->fieldName ) != 0 );
                }
                uassert( "Modifier $inc allowed for numbers only", f.isNumber() || op != Mod::INC );
                uassert( "Modifier $pushAll/pullAll allowed for arrays only", f.type() == Array || ( op != Mod::PUSH_ALL && op != Mod::PULL_ALL ) );
                m.elt = f;

                // horrible - to be cleaned up
                if ( f.type() == NumberDouble ) {
                    m.ndouble = (double *) f.value();
                    m.nint = 0;
                } else if ( f.type() == NumberInt ) {
                    m.ndouble = 0;
                    m.nint = (int *) f.value();
                }
                else if( f.type() == NumberLong ) { 
                    m.ndouble = 0;
                    m.nint = 0;
                    m.nlong = (long long *) f.value();
                }
                _mods.push_back( m );
            }
        }
    }

    void checkNoMods( BSONObj o ) {
        BSONObjIterator i( o );
        while( i.moreWithEOO() ) {
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
        int profile = cc().database()->profile;
        
        uassert("cannot update reserved $ collection", strchr(ns, '$') == 0 );
        if ( strstr(ns, ".system.") ) {
            /* dm: it's very important that system.indexes is never updated as IndexDetails has pointers into it */
            uassert("cannot update system collection", legalClientSystemNS( ns , true ) );
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
                        if ( mods.haveArrayDepMod() ) {
                            BSONObjBuilder patternBuilder;
                            patternBuilder.appendElements( pattern );
                            mods.appendSizeSpecForArrayDepMods( patternBuilder );
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
            checkNoMods( updateobj );
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
     
    /* multi means multiple updates. this is not implemented yet, but stubbing out for future work */   
    /* todo - clean up these crazy __updateobjects return codes! */
    bool updateObjects(const char *ns, BSONObj updateobj, BSONObj pattern, bool upsert, stringstream& ss, bool multi) {
        uassert("multi not coded yet", !multi);
        int rc = __updateObjects(ns, updateobj, pattern, upsert, ss, true);
        /* todo: why is there a logOp here when __updateObjects also does a bunch of logOps? */
        if ( rc != 5 && rc != 0 && rc != 4 && rc != 3 )
            logOp("u", ns, updateobj, &pattern, &upsert);
        return ( rc == 1 || rc == 2 || rc == 5 );
    }
}
