// update.h

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

#include "../stdafx.h"
#include "jsobj.h"
#include "../util/embedded_builder.h"
#include "matcher.h"

namespace mongo {

    /* Used for modifiers such as $inc, $set, ... */
    struct Mod {
        // See opFromStr below
        //        0    1    2     3         4     5          6    7      8       9       10
        enum Op { INC, SET, PUSH, PUSH_ALL, PULL, PULL_ALL , POP, UNSET, BITAND, BITOR , BIT  } op;
        
        static const char* modNames[];
        static unsigned modNamesNum;

        const char *fieldName;
        const char *shortFieldName;
        
        // kind of lame; fix one day?
        double *ndouble;
        int *nint;
        long long *nlong;

        BSONElement elt; // x:5 note: this is the actual element from the updateobj
        int pushStartSize;
        boost::shared_ptr<JSMatcher> matcher;

        void init( Op o , BSONElement& e ){
            op = o;
            elt = e;
            if ( op == PULL && e.type() == Object )
                matcher.reset( new JSMatcher( e.embeddedObject() ) );
        }

        void setFieldName( const char * s ){
            fieldName = s;
            shortFieldName = strrchr( fieldName , '.' );
            if ( shortFieldName )
                shortFieldName++;
            else
                shortFieldName = fieldName;
        }

        /* [dm] why is this const? (or rather, why was setn const?)  i see why but think maybe clearer if were not.  */
        void inc(BSONElement& n) const { 
            uassert( "$inc value is not a number", n.isNumber() );
            if( ndouble ) 
                *ndouble += n.numberDouble();
            else if( nint )
                *nint += n.numberInt();
            else
                *nlong += n.numberLong();
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
        
        bool isIndexed( const set<string>& idxKeys ) const {
            // check if there is an index key that is a parent of mod
            for( const char *dot = strchr( fieldName, '.' ); dot; dot = strchr( dot + 1, '.' ) )
                if ( idxKeys.count( string( fieldName, dot - fieldName ) ) )
                    return true;
            string fullName = fieldName;
            // check if there is an index key equal to mod
            if ( idxKeys.count(fullName) )
                return true;
            // check if there is an index key that is a child of mod
            set< string >::const_iterator j = idxKeys.upper_bound( fullName );
            if ( j != idxKeys.end() && j->find( fullName ) == 0 && (*j)[fullName.size()] == '.' )
                return true;
            return false;
        }
        
        void apply( BSONObjBuilder& b , BSONElement in );
        
        /**
         * @return true iff toMatch should be removed from the array
         */
        bool _pullElementMatch( BSONElement& toMatch ) const;

        bool needOpLogRewrite() const {
            switch( op ){
            case BIT:
            case BITAND:
            case BITOR:
                // TODO: should we convert this to $set?
                return false;
            default:
                return false;
            }
        }
        
        void appendForOpLog( BSONObjBuilder& b ) const {
            const char * name = modNames[op];
            
            BSONObjBuilder bb( b.subobjStart( name ) );
            bb.append( elt );
            bb.done();
        }
    };

    class ModSet {
        typedef map<string,Mod> ModHolder;
        ModHolder _mods;
        
        static void extractFields( map< string, BSONElement > &fields, const BSONElement &top, const string &base );
        
        FieldCompareResult compare( const ModHolder::iterator &m, map< string, BSONElement >::iterator &p, const map< string, BSONElement >::iterator &pEnd ) const {
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

            return compareDottedFieldNames( m->first, p->first.c_str() );
        }

        void _appendNewFromMods( const string& root , Mod& m , BSONObjBuilder& b , set<string>& onedownseen );
        
        void appendNewFromMod( Mod& m , BSONObjBuilder& b ){
            
            switch ( m.op ){
                
            case Mod::PUSH: { 
                BSONObjBuilder arr( b.subarrayStart( m.shortFieldName ) );
                arr.appendAs( m.elt, "0" );
                arr.done();
                m.pushStartSize = -1;
                break;
            } 
                
            case Mod::PUSH_ALL: {
                b.appendAs( m.elt, m.shortFieldName );
                m.pushStartSize = -1;
                break;
            } 
                
            case Mod::UNSET:
            case Mod::PULL:
            case Mod::PULL_ALL:
                // no-op b/c unset/pull of nothing does nothing
                break;
                
            case Mod::INC:
            case Mod::SET: {
                b.appendAs( m.elt, m.shortFieldName );
                break;
            }
            default: 
                stringstream ss;
                ss << "unknown mod in appendNewFromMod: " << m.op;
                throw UserException( ss.str() );
            }
         
        }
        
        bool mayAddEmbedded( map< string, BSONElement > &existing, string right ) {
            for( string left = EmbeddedBuilder::splitDot( right );
                 left.length() > 0 && left[ left.length() - 1 ] != '.';
                 left += "." + EmbeddedBuilder::splitDot( right ) ) {
                if ( existing.count( left ) > 0 && existing[ left ].type() != Object )
                    return false;
                if ( haveModForField( left.c_str() ) )
                    return false;
            }
            return true;
        }
        static Mod::Op opFromStr( const char *fn ) {
            for( unsigned i=0; i<Mod::modNamesNum; i++ )
                if ( strcmp( fn, Mod::modNames[ i ] ) == 0 )
                    return Mod::Op( i );
            
            uassert( "Invalid modifier specified " + string( fn ), false );
            return Mod::INC;
        }
        
    public:

        void getMods( const BSONObj &from );
        /**
           will return if can be done in place, or uassert if there is an error
           @return whether or not the mods can be done in place
         */
        bool canApplyInPlaceAndVerify( const BSONObj &obj ) const;
        void applyModsInPlace( const BSONObj &obj ) const;

        // new recursive version, will replace at some point
        void createNewFromMods( const string& root , BSONObjBuilder& b , const BSONObj &obj );

        BSONObj createNewFromMods( const BSONObj &obj );

        BSONObj createNewFromQuery( const BSONObj& query );

        /**
         *
         */
        int isIndexed( const set<string>& idxKeys ) const {
            int numIndexes = 0;
            for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); i++ ){
                if ( i->second.isIndexed( idxKeys ) )
                    numIndexes++;
            }
            return numIndexes;
        }

        unsigned size() const { return _mods.size(); }

        bool haveModForField( const char *fieldName ) const {
            return _mods.find( fieldName ) != _mods.end();
        }

        bool haveConflictingMod( const string& fieldName ){
            size_t idx = fieldName.find( '.' );
            if ( idx == string::npos )
                idx = fieldName.size();
            
            ModHolder::const_iterator start = _mods.lower_bound(fieldName.substr(0,idx));
            for ( ; start != _mods.end(); start++ ){
                FieldCompareResult r = compareDottedFieldNames( fieldName , start->first );
                switch ( r ){
                case LEFT_SUBFIELD: return true;
                case LEFT_BEFORE: return false;
                case SAME: return true;
                case RIGHT_BEFORE: return false;
                case RIGHT_SUBFIELD: return true;
                }
            }
            return false;

            
        }
        
        // re-writing for oplog

        bool needOpLogRewrite() const {
            for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); i++ )
                if ( i->second.needOpLogRewrite() )
                    return true;
            return false;            
        }
        
        BSONObj getOpLogRewrite() const {
            BSONObjBuilder b;
            for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); i++ )
                i->second.appendForOpLog( b );
            return b.obj();
        }

        bool haveArrayDepMod() const {
            for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); i++ )
                if ( i->second.arrayDep() )
                    return true;
            return false;
        }

        void appendSizeSpecForArrayDepMods( BSONObjBuilder &b ) const {
            for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); i++ ) {
                const Mod& m = i->second;
                if ( m.arrayDep() ){
                    if ( m.pushStartSize == -1 )
                        b.appendNull( m.fieldName );
                    else
                        b << m.fieldName << BSON( "$size" << m.pushStartSize );
                }
            }
        }
    };
    

}

