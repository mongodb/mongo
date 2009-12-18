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

    bool Mod::_pullElementMatch( BSONElement& toMatch ) const {
        
        if ( elt.type() != Object ){
            // if elt isn't an object, then comparison will work
            return toMatch.woCompare( elt , false ) == 0;
        }

        if ( toMatch.type() != Object ){
            // looking for an object, so this can't match
            return false;
        }
        
        // now we have an object on both sides
        return matcher->matches( toMatch.embeddedObject() );
    }

    void Mod::apply( BSONObjBuilder& b , BSONElement in ){
        switch ( op ){
        
        case INC: {
            // TODO: this is horrible
            inc( in );
            b.appendAs( elt , shortFieldName ); 
            break;
        }
            
        case SET: {
            b.appendAs( elt , shortFieldName );
            break;
        }

        case UNSET: {
            //Explicit NOOP
            break;
        }

        case PUSH: {
            uassert( "$push can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
            BSONObjIterator i( in.embeddedObject() );
            int n=0;
            while ( i.more() ){
                bb.append( i.next() );
                n++;
            }

            pushStartSize = n;

            bb.appendAs( elt ,  bb.numStr( n ) );
            bb.done();
            break;
        }
            
        case PUSH_ALL: {
            uassert( "$pushAll can only be applied to an array" , in.type() == Array );
            uassert( "$pushAll has to be passed an array" , elt.type() );

            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
            
            BSONObjIterator i( in.embeddedObject() );
            int n=0;
            while ( i.more() ){
                bb.append( i.next() );
                n++;
            }

            pushStartSize = n;

            i = BSONObjIterator( elt.embeddedObject() );
            while ( i.more() ){
                bb.appendAs( i.next() , bb.numStr( n++ ) );
            }

            bb.done();
            break;
        }
            
        case PULL:
        case PULL_ALL: {
            uassert( "$pull/$pullAll can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
                        
            int n = 0;

            BSONObjIterator i( in.embeddedObject() );
            while ( i.more() ){
                BSONElement e = i.next();
                bool allowed = true;

                if ( op == PULL ){
                    allowed = ! _pullElementMatch( e );
                }
                else {
                    BSONObjIterator j( elt.embeddedObject() );
                    while( j.more() ) {
                        BSONElement arrJ = j.next();
                        if ( e.woCompare( arrJ, false ) == 0 ){
                            allowed = false;
                            break;
                        }
                    }
                }

                if ( allowed )
                    bb.appendAs( e , bb.numStr( n++ ) );
            }
            
            bb.done();
            break;
        }

        case POP: {
            uassert( "$pop can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
                        
            int n = 0;

            BSONObjIterator i( in.embeddedObject() );
            if ( elt.isNumber() && elt.number() < 0 ){
                // pop from front
                if ( i.more() ){
                    i.next();
                    n++;
                }

                while( i.more() ) {
                    bb.appendAs( i.next() , bb.numStr( n - 1 ).c_str() );
                    n++;
                }
            }
            else {
                // pop from back
                while( i.more() ) {
                    n++;
                    BSONElement arrI = i.next();
                    if ( i.more() ){
                        bb.append( arrI );
                    }
                }
            }

            pushStartSize = n;
            assert( pushStartSize == in.embeddedObject().nFields() );
            bb.done();
            break;
        }
            
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
                inPlacePossible = (m.op == Mod::UNSET);
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
                            if ( m._pullElementMatch( arrI ) )
                                inPlacePossible = false;
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
            case Mod::UNSET:
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
        else {
            appendNewFromMod( m , b );
        }
        
    }

    void ModSet::createNewFromMods( const string& root , BSONObjBuilder& b , const BSONObj &obj ){
        BSONObjIteratorSorted es( obj );
        BSONElement e = es.next();

        ModHolder::iterator m = _mods.lower_bound( root );
        ModHolder::iterator mend = _mods.lower_bound( root + "{" );

        set<string> onedownseen;
        
        while ( e.type() && m != mend ){
            string field = root + e.fieldName();
            FieldCompareResult cmp = compareDottedFieldNames( m->second.fieldName , field );

            switch ( cmp ){
                
            case LEFT_SUBFIELD: { // Mod is embeddeed under this element
                uassert( "LEFT_SUBFIELD only supports Object" , e.type() == Object || e.type() == Array );
                BSONObjBuilder bb ( e.type() == Object ? b.subobjStart( e.fieldName() ) : b.subarrayStart( e.fieldName() ) );
                stringstream nr; nr << root << e.fieldName() << ".";
                createNewFromMods( nr.str() , bb , e.embeddedObject() );
                bb.done();
                // inc both as we handled both
                e = es.next();
                m++;
                continue;
            }
            case LEFT_BEFORE: // Mod on a field that doesn't exist
                _appendNewFromMods( root , m->second , b , onedownseen );
                m++;
                continue;
            case SAME:
                m->second.apply( b , e );
                e = es.next();
                m++;
                continue;
            case RIGHT_BEFORE: // field that doesn't have a MOD
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
        
        // finished looping the mods, just adding the rest of the elements
        while ( e.type() ){
            b.append( e );
            e = es.next();
        }
        
        // do mods that don't have fields already
        for ( ; m != mend; m++ ){
            _appendNewFromMods( root , m->second , b , onedownseen );
        }
    }
            
    BSONObj ModSet::createNewFromMods( const BSONObj &obj ) {
        BSONObjBuilder b( (int)(obj.objsize() * 1.1) );
        createNewFromMods( "" , b , obj );
        return b.obj();
    }

    BSONObj ModSet::createNewFromQuery( const BSONObj& query ){
        BSONObj newObj;

        {
            BSONObjBuilder bb;
            BSONObjIterator i( query );
            while ( i.more() ){
                BSONElement e = i.next();

                if ( e.type() == Object && e.embeddedObject().firstElement().fieldName()[0] == '$' ){
                    // this means this is a $gt type filter, so don't make part of the new object
                    continue;
                }

                uassert( "upsert with foo.bar type queries not supported yet" , strchr( e.fieldName() , '.' ) == 0 );


                bb.append( e );
            }
            newObj = bb.obj();
        }
        
        if ( canApplyInPlaceAndVerify( newObj ) )
            applyModsInPlace( newObj );
        else
            newObj = createNewFromMods( newObj );
        
        return newObj;
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

                const char * fieldName = f.fieldName();

                uassert( "Mod on _id not allowed", strcmp( fieldName, "_id" ) != 0 );
                uassert( "Invalid mod field name, may not end in a period", fieldName[ strlen( fieldName ) - 1 ] != '.' );
                uassert( "Field name duplication not allowed with modifiers", ! haveModForField( fieldName ) );
                uassert( "have conflict mod" , ! haveConflictingMod( fieldName ) );
                uassert( "Modifier $inc allowed for numbers only", f.isNumber() || op != Mod::INC );
                uassert( "Modifier $pushAll/pullAll allowed for arrays only", f.type() == Array || ( op != Mod::PUSH_ALL && op != Mod::PULL_ALL ) );

                Mod m;
                m.init( op , f );
                m.setFieldName( f.fieldName() );

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
            uassert( "Modifiers and non-modifiers cannot be mixed", e.fieldName()[ 0 ] != '$' );
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
                NamespaceDetailsTransient& ndt = NamespaceDetailsTransient::get_w(ns);
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
                 
                BSONObj newObj = mods.createNewFromQuery( patternOrig );

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
