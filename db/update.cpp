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

#include "pch.h"
#include "query.h"
#include "pdfile.h"
#include "jsobjmanipulator.h"
#include "queryoptimizer.h"
#include "repl.h"
#include "update.h"

//#define DEBUGUPDATE(x) cout << x << endl;
#define DEBUGUPDATE(x)

namespace mongo {

    const char* Mod::modNames[] = { "$inc", "$set", "$push", "$pushAll", "$pull", "$pullAll" , "$pop", "$unset" ,
                                    "$bitand" , "$bitor" , "$bit" , "$addToSet" };
    unsigned Mod::modNamesNum = sizeof(Mod::modNames)/sizeof(char*);

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

    template< class Builder >
    void Mod::appendIncremented( Builder& bb , const BSONElement& in, ModState& ms ) const {
        BSONType a = in.type();
        BSONType b = elt.type();
        
        if ( a == NumberDouble || b == NumberDouble ){
            ms.incType = NumberDouble;
            ms.incdouble = elt.numberDouble() + in.numberDouble();
        }
        else if ( a == NumberLong || b == NumberLong ){
            ms.incType = NumberLong;
            ms.inclong = elt.numberLong() + in.numberLong();
        }
        else {
            ms.incType = NumberInt;
            ms.incint = elt.numberInt() + in.numberInt();
        }
        
        ms.appendIncValue( bb );
    }

    template< class Builder >
    void appendUnset( Builder &b ) {
    }
    
    template<>
    void appendUnset( BSONArrayBuilder &b ) {
        b.appendNull();
    }
    
    template< class Builder >
    void Mod::apply( Builder& b , BSONElement in , ModState& ms ) const {
        switch ( op ){
        
        case INC: {
            appendIncremented( b , in , ms );
            break;
        }
            
        case SET: {
            _checkForAppending( elt );
            b.appendAs( elt , shortFieldName );
            break;
        }

        case UNSET: {
            appendUnset( b );
            break;
        }
            
        case PUSH: {
            uassert( 10131 ,  "$push can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
            BSONObjIterator i( in.embeddedObject() );
            int n=0;
            while ( i.more() ){
                bb.append( i.next() );
                n++;
            }

            ms.pushStartSize = n;

            bb.appendAs( elt ,  bb.numStr( n ) );
            bb.done();
            break;
        }
            
        case ADDTOSET: {
            uassert( 12592 ,  "$addToSet can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
            
            BSONObjIterator i( in.embeddedObject() );
            int n=0;            

            if ( isEach() ){
                
                BSONElementSet toadd;
                parseEach( toadd );
                
                while ( i.more() ){
                    BSONElement cur = i.next();
                    bb.append( cur );
                    n++;           
                    toadd.erase( cur );
                }
                
                for ( BSONElementSet::iterator j=toadd.begin(); j!=toadd.end(); j++ ){
                    bb.appendAs( *j , BSONObjBuilder::numStr( n++ ) );
                }

            }
            else {

                bool found = false;

                while ( i.more() ){
                    BSONElement cur = i.next();
                    bb.append( cur );
                    n++;
                    if ( elt.woCompare( cur , false ) == 0 )
                        found = true;
                }
                
                if ( ! found )
                    bb.appendAs( elt ,  bb.numStr( n ) );
                
            }
            
            bb.done();
            break;
        }


            
        case PUSH_ALL: {
            uassert( 10132 ,  "$pushAll can only be applied to an array" , in.type() == Array );
            uassert( 10133 ,  "$pushAll has to be passed an array" , elt.type() );

            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
            
            BSONObjIterator i( in.embeddedObject() );
            int n=0;
            while ( i.more() ){
                bb.append( i.next() );
                n++;
            }

            ms.pushStartSize = n;

            i = BSONObjIterator( elt.embeddedObject() );
            while ( i.more() ){
                bb.appendAs( i.next() , bb.numStr( n++ ) );
            }

            bb.done();
            break;
        }
            
        case PULL:
        case PULL_ALL: {
            uassert( 10134 ,  "$pull/$pullAll can only be applied to an array" , in.type() == Array );
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
            uassert( 10135 ,  "$pop can only be applied to an array" , in.type() == Array );
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

            ms.pushStartSize = n;
            assert( ms.pushStartSize == in.embeddedObject().nFields() );
            bb.done();
            break;
        }

        case BIT: {
            uassert( 10136 ,  "$bit needs an array" , elt.type() == Object );
            uassert( 10137 ,  "$bit can only be applied to numbers" , in.isNumber() );
            uassert( 10138 ,  "$bit can't use a double" , in.type() != NumberDouble );
            
            int x = in.numberInt();
            long long y = in.numberLong();

            BSONObjIterator it( elt.embeddedObject() );
            while ( it.more() ){
                BSONElement e = it.next();
                uassert( 10139 ,  "$bit field must be number" , e.isNumber() );
                if ( strcmp( e.fieldName() , "and" ) == 0 ){
                    switch( in.type() ){
                    case NumberInt: x = x&e.numberInt(); break;
                    case NumberLong: y = y&e.numberLong(); break;
                    default: assert( 0 );
                    }
                }
                else if ( strcmp( e.fieldName() , "or" ) == 0 ){
                    switch( in.type() ){
                    case NumberInt: x = x|e.numberInt(); break;
                    case NumberLong: y = y|e.numberLong(); break;
                    default: assert( 0 );
                    }
                }

                else {
                    throw UserException( 9016, (string)"unknown bit mod:" + e.fieldName() );
                }
            }
            
            switch( in.type() ){
            case NumberInt: b.append( shortFieldName , x ); break;
            case NumberLong: b.append( shortFieldName , y ); break;
            default: assert( 0 );
            }

            break;
        }

        default:
            stringstream ss;
            ss << "Mod::apply can't handle type: " << op;
            throw UserException( 9017, ss.str() );
        }
    }

    auto_ptr<ModSetState> ModSet::prepare(const BSONObj &obj) const {
        ModSetState * mss = new ModSetState( obj );

        // Perform this check first, so that we don't leave a partially modified object on uassert.
        for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            DEBUGUPDATE( "\t\t prepare : " << i->first );
            ModState& ms = mss->_mods[i->first];

            const Mod& m = i->second;
            BSONElement e = obj.getFieldDotted(m.fieldName);
            
            ms.m = &m;
            ms.old = e;

            if ( e.eoo() ) {
                mss->amIInPlacePossible( m.op == Mod::UNSET );
                continue;
            } 
            
            switch( m.op ) {
            case Mod::INC:
                uassert( 10140 ,  "Cannot apply $inc modifier to non-number", e.isNumber() || e.eoo() );
                if ( mss->amIInPlacePossible( e.isNumber() ) ){
                    // check more typing info here
                    if ( m.elt.type() != e.type() ){
                        // if i'm incrememnting with a double, then the storage has to be a double
                        mss->amIInPlacePossible( m.elt.type() != NumberDouble ); 
                    }
                }
                break;

            case Mod::SET:
                mss->amIInPlacePossible( m.elt.type() == e.type() &&
                                         m.elt.valuesize() == e.valuesize() );
                break;
            
            case Mod::PUSH:
            case Mod::PUSH_ALL:
                uassert( 10141 ,  "Cannot apply $push/$pushAll modifier to non-array", e.type() == Array || e.eoo() );
                mss->amIInPlacePossible( false );
                break;

            case Mod::PULL:
            case Mod::PULL_ALL: {
                uassert( 10142 ,  "Cannot apply $pull/$pullAll modifier to non-array", e.type() == Array || e.eoo() );
                BSONObjIterator i( e.embeddedObject() );
                while( mss->_inPlacePossible && i.more() ) {
                    BSONElement arrI = i.next();
                    if ( m.op == Mod::PULL ) {
                        mss->amIInPlacePossible( ! m._pullElementMatch( arrI ) );
                    } 
                    else if ( m.op == Mod::PULL_ALL ) {
                        BSONObjIterator j( m.elt.embeddedObject() );
                        while( mss->_inPlacePossible && j.moreWithEOO() ) {
                            BSONElement arrJ = j.next();
                            if ( arrJ.eoo() )
                                break;
                            mss->amIInPlacePossible( arrI.woCompare( arrJ, false ) );
                        }
                    }
                }
                break;
            }

            case Mod::POP: {
                uassert( 10143 ,  "Cannot apply $pop modifier to non-array", e.type() == Array || e.eoo() );
                mss->amIInPlacePossible( e.embeddedObject().isEmpty() );
                break;
            }
                
            case Mod::ADDTOSET: {
                uassert( 12591 ,  "Cannot apply $addToSet modifier to non-array", e.type() == Array || e.eoo() );
                
                BSONObjIterator i( e.embeddedObject() );
                if ( m.isEach() ){
                    BSONElementSet toadd;
                    m.parseEach( toadd );
                    while( i.more() ) {
                        BSONElement arrI = i.next();
                        toadd.erase( arrI );
                    }
                    mss->amIInPlacePossible( toadd.size() == 0 );
                }
                else {
                    bool found = false;
                    while( i.more() ) {
                        BSONElement arrI = i.next();
                        if ( arrI.woCompare( m.elt , false ) == 0 ){
                            found = true;
                            break;
                        }
                    }
                    mss->amIInPlacePossible( found );
                }
                break;
            }
                
            default:
                // mods we don't know about shouldn't be done in place
                mss->amIInPlacePossible( false );
            }
        }
        
        return auto_ptr<ModSetState>( mss );
    }
    
    void ModSetState::applyModsInPlace() {
        for ( ModStateHolder::iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            ModState& m = i->second;
            
            switch ( m.m->op ){
            case Mod::UNSET:
            case Mod::PULL:
            case Mod::PULL_ALL:
            case Mod::ADDTOSET:
                // this should have been handled by prepare
                break;

            // [dm] the BSONElementManipulator statements below are for replication (correct?)
            case Mod::INC:
                m.m->incrementMe( m.old );
                m.fixedOpName = "$set";
                m.fixed = &(m.old);
                break;
            case Mod::SET:
                BSONElementManipulator( m.old ).replaceTypeAndValue( m.m->elt );
                break;
            default:
                uassert( 10144 ,  "can't apply mod in place - shouldn't have gotten here" , 0 );
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
    
    template< class Builder >
    void ModSetState::_appendNewFromMods( const string& root , ModState& m , Builder& b , set<string>& onedownseen ){
        const char * temp = m.fieldName();
        temp += root.size();
        const char * dot = strchr( temp , '.' );
        if ( dot ){
            string nr( m.fieldName() , 0 , 1 + ( dot - m.fieldName() ) );
            string nf( temp , 0 , dot - temp );
            if ( onedownseen.count( nf ) )
                return;
            onedownseen.insert( nf );
            BSONObjBuilder bb ( b.subobjStart( nf.c_str() ) );
            createNewFromMods( nr , bb , BSONObj() ); // don't infer an array from name
            bb.done();
        }
        else {
            appendNewFromMod( m , b );
        }
        
    }
    
    template< class Builder >
    void ModSetState::createNewFromMods( const string& root , Builder& b , const BSONObj &obj ){
        DEBUGUPDATE( "\t\t createNewFromMods root: " << root );
        BSONObjIteratorSorted es( obj );
        BSONElement e = es.next();

        ModStateHolder::iterator m = _mods.lower_bound( root );
        ModStateHolder::iterator mend = _mods.lower_bound( root + '{' );

        set<string> onedownseen;
        
        while ( e.type() && m != mend ){
            string field = root + e.fieldName();
            FieldCompareResult cmp = compareDottedFieldNames( m->second.m->fieldName , field );

            DEBUGUPDATE( "\t\t\t" << field << "\t" << m->second.m->fieldName << "\t" << cmp );
            
            switch ( cmp ){
                
            case LEFT_SUBFIELD: { // Mod is embeddeed under this element
                uassert( 10145 ,  "LEFT_SUBFIELD only supports Object" , e.type() == Object || e.type() == Array );
                if ( onedownseen.count( e.fieldName() ) == 0 ){
                    onedownseen.insert( e.fieldName() );
                    if ( e.type() == Object ) {
                        BSONObjBuilder bb( b.subobjStart( e.fieldName() ) );
                        stringstream nr; nr << root << e.fieldName() << ".";
                        createNewFromMods( nr.str() , bb , e.embeddedObject() );
                        bb.done();                        
                    } else {
                        BSONArrayBuilder ba( b.subarrayStart( e.fieldName() ) );
                        stringstream nr; nr << root << e.fieldName() << ".";
                        createNewFromMods( nr.str() , ba , e.embeddedObject() );
                        ba.done();
                    }
                    // inc both as we handled both
                    e = es.next();
                    m++;
                }
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
                b.append( e ); // if array, ignore field name
                e = es.next();
                continue;
            case RIGHT_SUBFIELD:
                massert( 10399 ,  "ModSet::createNewFromMods - RIGHT_SUBFIELD should be impossible" , 0 ); 
                break;
            default:
                massert( 10400 ,  "unhandled case" , 0 );
            }
        }
        
        // finished looping the mods, just adding the rest of the elements
        while ( e.type() ){
            b.append( e );  // if array, ignore field name
            e = es.next();
        }
        
        // do mods that don't have fields already
        for ( ; m != mend; m++ ){
            _appendNewFromMods( root , m->second , b , onedownseen );
        }
    }

    BSONObj ModSetState::createNewFromMods() {
        BSONObjBuilder b( (int)(_obj.objsize() * 1.1) );
        createNewFromMods( "" , b , _obj );
        return b.obj();
    }

    BSONObj ModSet::createNewFromQuery( const BSONObj& query ){
        BSONObj newObj;

        {
            BSONObjBuilder bb;
            EmbeddedBuilder eb( &bb );
            BSONObjIteratorSorted i( query );
            while ( i.more() ){
                BSONElement e = i.next();

                if ( e.type() == Object && e.embeddedObject().firstElement().fieldName()[0] == '$' ){
                    // this means this is a $gt type filter, so don't make part of the new object
                    continue;
                }

                eb.appendAs( e , e.fieldName() );
            }
            eb.done();
            newObj = bb.obj();
        }
        
        auto_ptr<ModSetState> mss = prepare( newObj );

        if ( mss->canApplyInPlace() )
            mss->applyModsInPlace();
        else
            newObj = mss->createNewFromMods();
        
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
    ModSet::ModSet(
        const BSONObj &from , 
        const set<string>& idxKeys,
        const set<string> *backgroundKeys)
        : _isIndexed(0) , _hasDynamicArray( false ) {
        
        BSONObjIterator it(from);
        
        while ( it.more() ) {
            BSONElement e = it.next();
            const char *fn = e.fieldName();
            
            uassert( 10147 ,  "Invalid modifier specified" + string( fn ), e.type() == Object );
            BSONObj j = e.embeddedObject();
            
            BSONObjIterator jt(j);
            Mod::Op op = opFromStr( fn );

            while ( jt.more() ) {
                BSONElement f = jt.next(); // x:44

                const char * fieldName = f.fieldName();

                uassert( 10148 ,  "Mod on _id not allowed", strcmp( fieldName, "_id" ) != 0 );
                uassert( 10149 ,  "Invalid mod field name, may not end in a period", fieldName[ strlen( fieldName ) - 1 ] != '.' );
                uassert( 10150 ,  "Field name duplication not allowed with modifiers", ! haveModForField( fieldName ) );
                uassert( 10151 ,  "have conflict mod" , ! haveConflictingMod( fieldName ) );
                uassert( 10152 ,  "Modifier $inc allowed for numbers only", f.isNumber() || op != Mod::INC );
                uassert( 10153 ,  "Modifier $pushAll/pullAll allowed for arrays only", f.type() == Array || ( op != Mod::PUSH_ALL && op != Mod::PULL_ALL ) );
                
                _hasDynamicArray = _hasDynamicArray || strstr( fieldName , ".$" ) > 0;
                
                Mod m;
                m.init( op , f );
                m.setFieldName( f.fieldName() );
                
                if ( m.isIndexed( idxKeys ) ||
                    (backgroundKeys && m.isIndexed(*backgroundKeys)) ) {
                    _isIndexed++;
                }

                _mods[m.fieldName] = m;

                DEBUGUPDATE( "\t\t " << fieldName << "\t" << _hasDynamicArray );
            }
        }

    }

    ModSet * ModSet::fixDynamicArray( const char * elemMatchKey ) const {
        ModSet * n = new ModSet();
        n->_isIndexed = _isIndexed;
        n->_hasDynamicArray = _hasDynamicArray;
        for ( ModHolder::const_iterator i=_mods.begin(); i!=_mods.end(); i++ ){
            string s = i->first;
            size_t idx = s.find( ".$" );
            if ( idx == string::npos ){
                n->_mods[s] = i->second;
                continue;
            }
            StringBuilder buf(s.size()+strlen(elemMatchKey));
            buf << s.substr(0,idx+1) << elemMatchKey << s.substr(idx+2);
            string fixed = buf.str();
            DEBUGUPDATE( "fixed dynamic: " << s << " -->> " << fixed );
            n->_mods[fixed] = i->second;
            ModHolder::iterator temp = n->_mods.find( fixed );
            temp->second.setFieldName( temp->first.c_str() );
        }
        return n;
    }
    
    void checkNoMods( BSONObj o ) {
        BSONObjIterator i( o );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            uassert( 10154 ,  "Modifiers and non-modifiers cannot be mixed", e.fieldName()[ 0 ] != '$' );
        }
    }
    
    class UpdateOp : public QueryOp {
    public:
        UpdateOp( bool hasPositionalField ) : _nscanned(), _hasPositionalField( hasPositionalField ){}
        virtual void init() {
            BSONObj pattern = qp().query();
            _c = qp().newCursor();
            if ( ! _c->ok() )
                setComplete();
            else
                _matcher.reset( new CoveredIndexMatcher( pattern, qp().indexKey() , _hasPositionalField ) );
        }
        virtual void next() {
            if ( ! _c->ok() ) {
                setComplete();
                return;
            }
            _nscanned++;
            if ( _matcher->matches(_c->currKey(), _c->currLoc(), &_details ) ) {
                setComplete();
                return;
            }
            _c->advance();
        }
        bool curMatches(){
            return _matcher->matches(_c->currKey(), _c->currLoc() , &_details );
        }

        CoveredIndexMatcher* getMatcher(){
            return _matcher.get();
        }

        virtual bool mayRecordPlan() const { return false; }
        virtual QueryOp *clone() const {
            return new UpdateOp( _hasPositionalField );
        }
        
        shared_ptr< Cursor > c() { return _c; }
        long long nscanned() const { return _nscanned; }
        MatchDetails& getMatchDetails(){ return _details; }

    private:
        shared_ptr< Cursor > _c;
        long long _nscanned;
        bool _hasPositionalField;
        auto_ptr< CoveredIndexMatcher > _matcher;
        MatchDetails _details;
    };

    UpdateResult _updateObjects(bool god, const char *ns, const BSONObj& updateobj, BSONObj patternOrig, bool upsert, bool multi, bool logop , OpDebug& debug) {
        DEBUGUPDATE( "update: " << ns << " update: " << updateobj << " query: " << patternOrig << " upsert: " << upsert << " multi: " << multi );
        int profile = cc().database()->profile;
        StringBuilder& ss = debug.str;

        if ( logLevel > 2 )
            ss << " update: " << updateobj;
        
        /* idea with these here it to make them loop invariant for multi updates, and thus be a bit faster for that case */
        /* NOTE: when yield() is added herein, these must be refreshed after each call to yield! */
        NamespaceDetails *d = nsdetails(ns); // can be null if an upsert...
        NamespaceDetailsTransient *nsdt = &NamespaceDetailsTransient::get_w(ns);
        /* end note */
        
        auto_ptr<ModSet> mods;
        bool isOperatorUpdate = updateobj.firstElement().fieldName()[0] == '$';
        int modsIsIndexed = false; // really the # of indexes
        if ( isOperatorUpdate ){
            if( d && d->backgroundIndexBuildInProgress ) { 
                set<string> bgKeys;
                d->backgroundIdx().keyPattern().getFieldNames(bgKeys);
                mods.reset( new ModSet(updateobj, nsdt->indexKeys(), &bgKeys) );
            }
            else {
                mods.reset( new ModSet(updateobj, nsdt->indexKeys()) );
            }
            modsIsIndexed = mods->isIndexed();
        }

        set<DiskLoc> seenObjects;
        
        int numModded = 0;
        long long nscanned = 0;
        MultiPlanScanner mps( ns, patternOrig, BSONObj() );
        while( mps.mayRunMore() ) {
            UpdateOp original( mods.get() && mods->hasDynamicArray() );
            shared_ptr< UpdateOp > u = mps.runOpOnce( original );
            massert( 10401 ,  u->exceptionMessage(), u->complete() );
            
            shared_ptr< Cursor > c = u->c();
            auto_ptr<ClientCursor> cc;
            
            while ( c->ok() ) {
                nscanned++;
                
                if ( numModded > 0 && ! u->curMatches() ){
                    c->advance();
                    
                    if ( nscanned % 256 == 0 && ! u->getMatcher()->docMatcher().atomic() ){
                        if ( cc.get() == 0 )
                            cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , c , ns ) );
                        if ( ! cc->yield() ){
                            cc.release();
                            break;
                        }
                    }
                    
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
                        uassert( 10157 ,  "multi-update requires all modified objects to have an _id" , ! multi );
                    }
                }
                
                if ( profile )
                    ss << " nscanned:" << nscanned;
                
                /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
                 regular ones at the moment. */
                if ( isOperatorUpdate ) {
                    
                    if ( multi ){
                        c->advance(); // go to next record in case this one moves
                        if ( seenObjects.count( loc ) )
                            continue;
                    }
                    
                    const BSONObj& onDisk = loc.obj();
                    
                    ModSet * useMods = mods.get();
                    bool forceRewrite = false;
                    
                    auto_ptr<ModSet> mymodset;
                    if ( u->getMatchDetails().elemMatchKey && mods->hasDynamicArray() ){
                        useMods = mods->fixDynamicArray( u->getMatchDetails().elemMatchKey );
                        mymodset.reset( useMods );
                        forceRewrite = true;
                    }
                    
                    
                    auto_ptr<ModSetState> mss = useMods->prepare( onDisk );
                    
                    bool indexHack = multi && ( modsIsIndexed || ! mss->canApplyInPlace() );
                    
                    if ( indexHack )
                        c->noteLocation();
                    
                    if ( modsIsIndexed <= 0 && mss->canApplyInPlace() ){
                        mss->applyModsInPlace();// const_cast<BSONObj&>(onDisk) );
                        
                        if ( profile )
                            ss << " fastmod ";
                        
                        if ( modsIsIndexed ){
                            seenObjects.insert( loc );
                        }
                    } 
                    else {
                        BSONObj newObj = mss->createNewFromMods();
                        uassert( 12522 , "$ operator made object too large" , newObj.objsize() <= ( 4 * 1024 * 1024 ) );
                        bool changedId;
                        DiskLoc newLoc = theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , newObj.objdata(), newObj.objsize(), debug, changedId);
                        if ( newLoc != loc || modsIsIndexed ) {
                            // object moved, need to make sure we don' get again
                            seenObjects.insert( newLoc );
                        }
                        
                    }
                    
                    if ( logop ) {
                        DEV assert( mods->size() );
                        
                        if ( mss->haveArrayDepMod() ) {
                            BSONObjBuilder patternBuilder;
                            patternBuilder.appendElements( pattern );
                            mss->appendSizeSpecForArrayDepMods( patternBuilder );
                            pattern = patternBuilder.obj();                        
                        }
                        
                        if ( forceRewrite || mss->needOpLogRewrite() ){
                            DEBUGUPDATE( "\t rewrite update: " << mss->getOpLogRewrite() );
                            logOp("u", ns, mss->getOpLogRewrite() , &pattern );
                        }
                        else {
                            logOp("u", ns, updateobj, &pattern );
                        }
                    }
                    numModded++;
                    if ( ! multi )
                        return UpdateResult( 1 , 1 , numModded );
                    if ( indexHack )
                        c->checkLocation();
                    
                    if ( nscanned % 64 == 0 && ! u->getMatcher()->docMatcher().atomic() ){
                        if ( cc.get() == 0 )
                            cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , c , ns ) );
                        if ( ! cc->yield() ){
                            cc.release();
                            break;
                        }
                    }
                    
                    continue;
                } 
                
                uassert( 10158 ,  "multi update only works with $ operators" , ! multi );
                
                BSONElementManipulator::lookForTimestamps( updateobj );
                checkNoMods( updateobj );
                bool changedId = false;
                theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , updateobj.objdata(), updateobj.objsize(), debug, changedId);
                if ( logop ) {
                    if ( !changedId ) {
                        logOp("u", ns, updateobj, &pattern );
                    } else {
                        logOp("d", ns, pattern );
                        logOp("i", ns, updateobj );                    
                    }
                }
                return UpdateResult( 1 , 0 , 1 );
            }
        }
        
        if ( numModded )
            return UpdateResult( 1 , 1 , numModded );

        
        if ( profile )
            ss << " nscanned:" << nscanned;
        
        if ( upsert ) {
            if ( updateobj.firstElement().fieldName()[0] == '$' ) {
                /* upsert of an $inc. build a default */
                BSONObj newObj = mods->createNewFromQuery( patternOrig );
                if ( profile )
                    ss << " fastmodinsert ";
                theDataFileMgr.insertWithObjMod(ns, newObj, god);
                if ( profile )
                    ss << " fastmodinsert ";
                if ( logop )
                    logOp( "i", ns, newObj );
                return UpdateResult( 0 , 1 , 1 );
            }
            uassert( 10159 ,  "multi update only works with $ operators" , ! multi );
            checkNoMods( updateobj );
            if ( profile )
                ss << " upsert ";
            BSONObj no = updateobj;
            theDataFileMgr.insertWithObjMod(ns, no, god);
            if ( logop )
                logOp( "i", ns, no );
            return UpdateResult( 0 , 0 , 1 );
        }
        return UpdateResult( 0 , 0 , 0 );
    }
 
    UpdateResult updateObjects(const char *ns, const BSONObj& updateobj, BSONObj patternOrig, bool upsert, bool multi, bool logop , OpDebug& debug ) {
        uassert( 10155 , "cannot update reserved $ collection", strchr(ns, '$') == 0 );
        if ( strstr(ns, ".system.") ) {
            /* dm: it's very important that system.indexes is never updated as IndexDetails has pointers into it */
            uassert( 10156 , "cannot update system collection", legalClientSystemNS( ns , true ) );
        }
        return _updateObjects(false, ns, updateobj, patternOrig, upsert, multi, logop, debug);
    }
   
}
