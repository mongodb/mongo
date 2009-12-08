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
#include "update.h"

namespace mongo {

    void Mod::apply( BSONObjBuilder& b , BSONElement in ){
        switch ( op ){
        case INC:
            inc( in );
            b.appendAs( elt , shortFieldName ); // TODO: this is horrible
            break;
        default:
            stringstream ss;
            ss << "Mod::apply can't handle type: " << op;
            throw UserException( ss.str() );
        }
    }

    bool ModSet::canApplyInPlaceAndVerify(const BSONObj &obj) const {
        bool inPlacePossible = true;

        // Perform this check first, so that we don't leave a partially modified object on uassert.
        for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            const Mod& m = i->second;
            BSONElement e = obj.getFieldDotted(m.fieldName);
            
            if ( e.eoo() ) {
                inPlacePossible = false;
            } 
            else {
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
                    while( inPlacePossible && i.more() ) {
                        BSONElement arrI = i.next();
                        if ( m.op == Mod::PULL ) {
                            if ( arrI.woCompare( m.elt, false ) == 0 ) {
                                inPlacePossible = false;
                            }
                        } 
                        else if ( m.op == Mod::PULL_ALL ) {
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
                default:
                    // mods we don't know about shouldn't be done in place
                    inPlacePossible = false;
                }
            }
        }
        return inPlacePossible;
    }
    
    void ModSet::applyModsInPlace(const BSONObj &obj) const {
        for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            const Mod& m = i->second;
            BSONElement e = obj.getFieldDotted(m.fieldName);
            
            switch ( m.op ){
            case Mod::PULL:
            case Mod::PULL_ALL:
                break;

            // [dm] the BSONElementManipulator statements below are for replication (correct?)
            case Mod::INC:
                m.inc(e);
                m.setElementToOurNumericValue(e);
                break;
            case Mod::SET:
                if ( e.isNumber() && m.elt.isNumber() ) {
                    // todo: handle NumberLong:
                    m.setElementToOurNumericValue(e);
                } 
                else {
                    BSONElementManipulator( e ).replaceTypeAndValue( m.elt );
                }
                break;
            default:
                uassert( "can't apply mod in place - shouldn't have gotten here" , 0 );
            }
        }
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
    
    void ModSet::_appendNewFromMods( const string& root , Mod& m , BSONObjBuilder& b , set<string>& onedownseen ){
        const char * temp = m.fieldName;
        temp += root.size();
        const char * dot = strchr( temp , '.' );
        if ( dot ){
            string nr( m.fieldName , 0 , 1 + ( dot - m.fieldName ) );
            string nf( temp , 0 , dot - temp );
            if ( onedownseen.count( nf ) )
                return;
            onedownseen.insert( nf );
            BSONObjBuilder bb ( b.subobjStart( nf.c_str() ) );
            createNewFromMods( nr , bb , BSONObj() );
            bb.done();
        }
        else
            appendNewFromMod( m , b );
        
    }
    
    void ModSet::createNewFromMods( const string& root , BSONObjBuilder& b , const BSONObj &obj ){
        BSONObjIterator es( obj );
        BSONElement e = es.next();

        ModHolder::iterator m = _mods.lower_bound( root );
        ModHolder::iterator mend = _mods.lower_bound( root + "{" );

        set<string> onedownseen;
        list<Mod*> toadd; // TODO: remove.  this is a hack to make new and old impls. identical.  when testing is complete, we should remove

        while ( e.type() && m != mend ){
            string field = root + e.fieldName();
            FieldCompareResult cmp = compareDottedFieldNames( m->second.fieldName , field );

            switch ( cmp ){
            case LEFT_SUBFIELD: {
                uassert( "LEFT_SUBFIELD only supports Object" , e.type() == Object );
                BSONObjBuilder bb ( b.subobjStart( e.fieldName() ) );
                stringstream nr; nr << root << e.fieldName() << ".";
                createNewFromMods( nr.str() , bb , e.embeddedObject() );
                bb.done();
                // inc both as we handled both
                e = es.next();
                m++;
                continue;
            }
            case LEFT_BEFORE: 
                toadd.push_back( &(m->second) );
                m++;
                continue;
            case SAME:
                m->second.apply( b , e );
                e = es.next();
                m++;
                continue;
            case RIGHT_BEFORE:
                b.append( e );
                e = es.next();
                continue;
            case RIGHT_SUBFIELD:
                massert( "ModSet::createNewFromMods - RIGHT_SUBFIELD should be impossible" , 0 ); 
                break;
            default:
                massert( "unhandled case" , 0 );
            }
        }
        
        while ( e.type() ){
            b.append( e );
            e = es.next();
        }
        
        for ( list<Mod*>::iterator i=toadd.begin(); i!=toadd.end(); i++ )
            _appendNewFromMods( root , **i , b , onedownseen );


        for ( ; m != mend; m++ ){
            _appendNewFromMods( root , m->second , b , onedownseen );
        }
    }
            
    BSONObj ModSet::createNewFromMods_r( const BSONObj &obj ) {
        BSONObjBuilder b;
        createNewFromMods( "" , b , obj );
        return b.obj();
    }

    BSONObj ModSet::createNewFromMods_l( const BSONObj &obj ) {
        map< string, BSONElement > existing;
        
        BSONObjBuilder b;
        BSONObjIterator i( obj );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( ! haveModForFieldOrSubfield( e.fieldName() ) ) {
                b.append( e );
            } 
            else {
                extractFields( existing, e, "" );
            }
        }
            
        EmbeddedBuilder b2( &b );
        ModHolder::iterator m = _mods.begin();
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
                         mayAddEmbedded( existing, m->second.fieldName ) );                
                // $ modifier applied to missing field -- create field from scratch
                appendNewFromMod( m->second , b2 );
                m++;
                continue;
            }

            FieldCompareResult cmp = compareDottedFieldNames( m->second.fieldName , p->first );
            if ( cmp <= 0 )
                uassert( "Modifier spec implies existence of an encapsulating object with a name that already represents a non-object,"
                         " or is referenced in another $set clause",
                         mayAddEmbedded( existing, m->second.fieldName ) );                
            if ( cmp == 0 ) {
                BSONElement e = p->second;
                if ( m->second.op == Mod::INC ) {
                    m->second.inc(e);
                    //m->setn( m->getn() + e.number() );
                    b2.appendAs( m->second.elt, m->second.fieldName );
                } else if ( m->second.op == Mod::SET ) {
                    b2.appendAs( m->second.elt, m->second.fieldName );
                } else if ( m->second.op == Mod::PUSH || m->second.op == Mod::PUSH_ALL ) {
                    BSONObjBuilder arr( b2.subarrayStartAs( m->second.fieldName ) );
                    BSONObjIterator i( e.embeddedObject() );
                    int startCount = 0;
                    while( i.moreWithEOO() ) {
                        BSONElement arrI = i.next();
                        if ( arrI.eoo() )
                            break;
                        arr.append( arrI );
                        ++startCount;
                    }
                    if ( m->second.op == Mod::PUSH ) {
                        stringstream ss;
                        ss << startCount;
                        string nextIndex = ss.str();
                        arr.appendAs( m->second.elt, nextIndex.c_str() );
                    } else {
                        BSONObjIterator i( m->second.elt.embeddedObject() );
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
                    m->second.pushStartSize = startCount;
                } else if ( m->second.op == Mod::PULL || m->second.op == Mod::PULL_ALL ) {
                    BSONObjBuilder arr( b2.subarrayStartAs( m->second.fieldName ) );
                    BSONObjIterator i( e.embeddedObject() );
                    int count = 0;
                    while( i.moreWithEOO() ) {
                        BSONElement arrI = i.next();
                        if ( arrI.eoo() )
                            break;
                        bool allowed = true;
                        if ( m->second.op == Mod::PULL ) {
                            allowed = ( arrI.woCompare( m->second.elt, false ) != 0 );
                        } else {
                            BSONObjIterator j( m->second.elt.embeddedObject() );
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
                else if ( m->second.op == Mod::POP ){
                    int startCount = 0;
                    BSONObjBuilder arr( b2.subarrayStartAs( m->second.fieldName ) );
                    BSONObjIterator i( e.embeddedObject() );
                    if ( m->second.elt.isNumber() && m->second.elt.number() < 0 ){
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
                    m->second.pushStartSize = startCount;
                }
                ++m;
                ++p;
            } 
            else if ( cmp < 0 ) {
                // $ modifier applied to missing field -- create field from scratch
                appendNewFromMod( m->second , b2 );
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
        _sorted = false;
        BSONObjIterator it(from);
        while ( it.more() ) {
            BSONElement e = it.next();
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
                m.setFieldName( f.fieldName() );
                uassert( "Mod on _id not allowed", strcmp( m.fieldName, "_id" ) != 0 );
                uassert( "Invalid mod field name, may not end in a period", m.fieldName[ strlen( m.fieldName ) - 1 ] != '.' );
                uassert( "Field name duplication not allowed with modifiers", ! haveModForField( m.fieldName ) );

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
                _mods[m.fieldName] = m;
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
            c_.reset( qp().newCursor().release() );
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
        bool curMatches(){
            return matcher_->matches(c_->currKey(), c_->currLoc() );
        }
        virtual bool mayRecordPlan() const { return false; }
        virtual QueryOp *clone() const {
            return new UpdateOp();
        }
        shared_ptr< Cursor > c() { return c_; }
        long long nscanned() const { return nscanned_; }
    private:
        shared_ptr< Cursor > c_;
        long long nscanned_;
        auto_ptr< KeyValJSMatcher > matcher_;
    };

    
    UpdateResult updateObjects(const char *ns, BSONObj updateobjOrig, BSONObj patternOrig, bool upsert, bool multi, stringstream& ss, bool logop ) {
        int profile = cc().database()->profile;
        
        uassert("cannot update reserved $ collection", strchr(ns, '$') == 0 );
        if ( strstr(ns, ".system.") ) {
            /* dm: it's very important that system.indexes is never updated as IndexDetails has pointers into it */
            uassert("cannot update system collection", legalClientSystemNS( ns , true ) );
        }

        set<DiskLoc> seenObjects;
        
        QueryPlanSet qps( ns, patternOrig, BSONObj() );
        UpdateOp original;
        shared_ptr< UpdateOp > u = qps.runOp( original );
        massert( u->exceptionMessage(), u->complete() );
        shared_ptr< Cursor > c = u->c();
        int numModded = 0;
        while ( c->ok() ) {
            if ( numModded > 0 && ! u->curMatches() ){
                c->advance();
                continue;
            }
            Record *r = c->_current();
            DiskLoc loc = c->currLoc();

            if ( c->getsetdup( loc ) ){
                c->advance();
                continue;
            }
                               
            BSONObj js(r);
            
            BSONObj pattern = patternOrig;
            BSONObj updateobj = updateobjOrig;

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
                else {
                    uassert( "multi-update requires all modified objects to have an _id" , ! multi );
                }
            }
            
            if ( profile )
                ss << " nscanned:" << u->nscanned();
            
            /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
               regular ones at the moment. */
            
            const char *firstField = updateobj.firstElement().fieldName();
            
            if ( firstField[0] == '$' ) {

                if ( multi ){
                    c->advance(); // go to next record in case this one moves
                    if ( seenObjects.count( loc ) )
                        continue;
                    updateobj = updateobj.copy();
                }
                
                ModSet mods;
                mods.getMods(updateobj);
                NamespaceDetailsTransient& ndt = NamespaceDetailsTransient::get(ns);
                set<string>& idxKeys = ndt.indexKeys();
                int isIndexed = mods.isIndexed( idxKeys );
                
                if ( isIndexed && multi ){
                    c->noteLocation();
                }

                if ( isIndexed <= 0 && mods.canApplyInPlaceAndVerify( loc.obj() ) ) {
                    mods.applyModsInPlace( loc.obj() );
                    //seenObjects.insert( loc );
                    if ( profile )
                        ss << " fastmod ";
                    
                    if ( isIndexed ){
                        seenObjects.insert( loc );
                    }
                } 
                else {
                    BSONObj newObj = mods.createNewFromMods( loc.obj() );
                    DiskLoc newLoc = theDataFileMgr.update(ns, r, loc , newObj.objdata(), newObj.objsize(), ss);
                    if ( newLoc != loc || isIndexed ){
                        // object moved, need to make sure we don' get again
                        seenObjects.insert( newLoc );
                    }
                        
                }

                if ( logop ) {
                    if ( mods.size() ) {
                        if ( mods.haveArrayDepMod() ) {
                            BSONObjBuilder patternBuilder;
                            patternBuilder.appendElements( pattern );
                            mods.appendSizeSpecForArrayDepMods( patternBuilder );
                            pattern = patternBuilder.obj();                        
                        }
                    }
                    logOp("u", ns, updateobj, &pattern );
                }
                numModded++;
                if ( ! multi )
                    break;
                if ( multi && isIndexed )
                    c->checkLocation();
                continue;
            } 
            
            uassert( "multi update only works with $ operators" , ! multi );

            BSONElementManipulator::lookForTimestamps( updateobj );
            checkNoMods( updateobj );
            theDataFileMgr.update(ns, r, loc , updateobj.objdata(), updateobj.objsize(), ss);
            if ( logop )
                logOp("u", ns, updateobj, &pattern );
            return UpdateResult( 1 , 0 , 1 );
        }
        
        if ( numModded )
            return UpdateResult( 1 , 1 , numModded );

        
        if ( profile )
            ss << " nscanned:" << u->nscanned();
        
        if ( upsert ) {
            if ( updateobjOrig.firstElement().fieldName()[0] == '$' ) {
                /* upsert of an $inc. build a default */
                ModSet mods;
                mods.getMods(updateobjOrig);
                 
                BSONObj newObj = patternOrig.copy();
                if ( mods.canApplyInPlaceAndVerify( newObj ) )
                    mods.applyModsInPlace( newObj );
                else
                    newObj = mods.createNewFromMods( newObj );

                if ( profile )
                    ss << " fastmodinsert ";
                theDataFileMgr.insert(ns, newObj);
                if ( profile )
                    ss << " fastmodinsert ";
                if ( logop )
                    logOp( "i", ns, newObj );
                return UpdateResult( 0 , 1 , 1 );
            }
            uassert( "multi update only works with $ operators" , ! multi );
            checkNoMods( updateobjOrig );
            if ( profile )
                ss << " upsert ";
            theDataFileMgr.insert(ns, updateobjOrig);
            if ( logop )
                logOp( "i", ns, updateobjOrig );
            return UpdateResult( 0 , 0 , 1 );
        }
        return UpdateResult( 0 , 0 , 0 );
    }
    
}
