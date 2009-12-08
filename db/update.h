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

#include "stdafx.h"
#include "jsobj.h"
#include "../util/embedded_builder.h"


namespace mongo {

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
    };

    class ModSet {
        vector< Mod > _mods;
        bool _sorted;
        
        void sortMods() {
            if ( ! _sorted ){
                sort( _mods.begin(), _mods.end() );
                _sorted = true;
            }
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
        BSONObj createNewFromMods_r( const BSONObj &obj );

        BSONObj createNewFromMods( const BSONObj &obj ){
            return createNewFromMods_l( obj );
        }

        /**
         *
         */
        int isIndexed( const set<string>& idxKeys ) const {
            int numIndexes = 0;
            for ( vector<Mod>::const_iterator i = _mods.begin(); i != _mods.end(); i++ ){
                if ( i->isIndexed( idxKeys ) )
                    numIndexes++;
            }
            return numIndexes;
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
    

}

