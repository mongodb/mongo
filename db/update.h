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

namespace mongo {

    /* Used for modifiers such as $inc, $set, ... */
    struct Mod {
        enum Op { INC, SET, PUSH, PUSH_ALL, PULL, PULL_ALL , POP } op;
        const char *fieldName;
        const char *shortFieldName;
        
        // kind of lame; fix one day?
        double *ndouble;
        int *nint;
        long long *nlong;

        BSONElement elt;
        int pushStartSize;

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
    };

    class ModSet {
        typedef map<string,Mod> ModHolder;
        ModHolder _mods;
        bool _sorted;
        
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

     void appendNewFromMod( Mod& m , BSONObjBuilder& b ){
            if ( m.op == Mod::PUSH ) {
                /*
                BSONObjBuilder arr( b.subarrayStartAs( m.fieldName ) );
                arr.appendAs( m.elt, "0" );
                arr.done();
                m.pushStartSize = -1;
                */
                uassert( "appendNewFromMod push not done" , 0 );
            } 
            else if ( m.op == Mod::PUSH_ALL ) {
                //b.appendAs( m.elt, m.fieldName );
                //m.pushStartSize = -1;
                uassert( "appendNewFromMod push_all not done" , 0 );
            } 
            else if ( m.op == Mod::PULL || m.op == Mod::PULL_ALL ) {
            }
            else if ( m.op == Mod::INC || m.op == Mod::SET ){
                b.appendAs( m.elt, m.shortFieldName );
            }
            else {
                uassert( "unknonw mod" , 0 );
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
            const char *valid[] = { "$inc", "$set", "$push", "$pushAll", "$pull", "$pullAll" , "$pop" };
            for( int i = 0; i < 7; ++i )
                if ( strcmp( fn, valid[ i ] ) == 0 )
                    return Mod::Op( i );
            uassert( "Invalid modifier specified " + string( fn ), false );
            return Mod::INC;
        }
    public:
        ModSet() : _sorted(false){}

        void getMods( const BSONObj &from );
        /**
           will return if can be done in place, or uassert if there is an error
           @return whether or not the mods can be done in place
         */
        bool canApplyInPlaceAndVerify( const BSONObj &obj ) const;
        void applyModsInPlace( const BSONObj &obj ) const;

        // old linear version
        BSONObj createNewFromMods_l( const BSONObj &obj );

        // new recursive version, will replace at some point
        void createNewFromMods( const string& root , BSONObjBuilder& b , const BSONObj &obj );

        // new recursive version, will replace at some point
        BSONObj createNewFromMods_r( const BSONObj &obj );

        BSONObj createNewFromMods( const BSONObj &obj ){
            return createNewFromMods_l( obj );
        }

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
        bool haveModForFieldOrSubfield( const string& fieldName ) const {
            ModHolder::const_iterator start = _mods.lower_bound(fieldName);
            for ( ; start != _mods.end(); start++ ){
                FieldCompareResult r = compareDottedFieldNames( fieldName , start->first );
                switch ( r ){
                case LEFT_SUBFIELD: assert(0); break;
                case LEFT_BEFORE: continue;
                case SAME: return true;
                case RIGHT_BEFORE: continue;
                case RIGHT_SUBFIELD: return true;
                }
            }
            return false;
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

