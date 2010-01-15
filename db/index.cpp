// index.cpp

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
#include "namespace.h"
#include "index.h"
#include "btree.h"
#include "query.h"

namespace mongo {

    /* delete this index.  does NOT clean up the system catalog
       (system.indexes or system.namespaces) -- only NamespaceIndex.
    */
    void IndexDetails::kill_idx() {
        string ns = indexNamespace(); // e.g. foo.coll.$ts_1
        
        // clean up parent namespace index cache
        NamespaceDetailsTransient::get_w( parentNS().c_str() ).deletedIndex();

        BSONObjBuilder b;
        b.append("name", indexName().c_str());
        b.append("ns", parentNS().c_str());
        BSONObj cond = b.done(); // e.g.: { name: "ts_1", ns: "foo.coll" }

        /* important to catch exception here so we can finish cleanup below. */
        try { 
            btreeStore->drop(ns.c_str());
        }
        catch(DBException& ) { 
            log(2) << "IndexDetails::kill(): couldn't drop ns " << ns << endl;
        }
        head.setInvalid();
        info.setInvalid();

        // clean up in system.indexes.  we do this last on purpose.  note we have 
        // to make the cond object before the drop() above though.
        string system_indexes = cc().database()->name + ".system.indexes";
        int n = deleteObjects(system_indexes.c_str(), cond, false, false, true);
        wassert( n == 1 );
    }

    void getKeys( vector< const char * > fieldNames, vector< BSONElement > fixed, const BSONObj &obj, BSONObjSetDefaultOrder &keys ) {
        BSONObjBuilder b;
        b.appendNull( "" );
        BSONElement nullElt = b.done().firstElement();
        BSONElement arrElt;
        unsigned arrIdx = ~0;
        for( unsigned i = 0; i < fieldNames.size(); ++i ) {
            if ( *fieldNames[ i ] == '\0' )
                continue;
            BSONElement e = obj.getFieldDottedOrArray( fieldNames[ i ] );
            if ( e.eoo() )
                e = nullElt; // no matching field
            if ( e.type() != Array )
                fieldNames[ i ] = ""; // no matching field or non-array match
            if ( *fieldNames[ i ] == '\0' )
                fixed[ i ] = e; // no need for further object expansion (though array expansion still possible)
            if ( e.type() == Array && arrElt.eoo() ) { // we only expand arrays on a single path -- track the path here
                arrIdx = i;
                arrElt = e;
            }
            // enforce single array path here
            uassert( 10088 ,  "cannot index parallel arrays", e.type() != Array || e.rawdata() == arrElt.rawdata() );
        }
        bool allFound = true; // have we found elements for all field names in the key spec?
        for( vector< const char * >::const_iterator i = fieldNames.begin(); allFound && i != fieldNames.end(); ++i )
            if ( **i != '\0' )
                allFound = false;
        if ( allFound ) {
            if ( arrElt.eoo() ) {
                // no terminal array element to expand
                BSONObjBuilder b;
                for( vector< BSONElement >::iterator i = fixed.begin(); i != fixed.end(); ++i )
                    b.appendAs( *i, "" );
                keys.insert( b.obj() );
            } else {
                // terminal array element to expand, so generate all keys
                BSONObjIterator i( arrElt.embeddedObject() );
                if ( i.more() ){
                    while( i.more() ) {
                        BSONObjBuilder b;
                        for( unsigned j = 0; j < fixed.size(); ++j ) {
                            if ( j == arrIdx )
                                b.appendAs( i.next(), "" );
                            else
                                b.appendAs( fixed[ j ], "" );
                        }
                        keys.insert( b.obj() );
                    }
                }
                else if ( fixed.size() > 1 ){
                    // x : [] - need to insert undefined
                    BSONObjBuilder b;
                    for( unsigned j = 0; j < fixed.size(); ++j ) {
                        if ( j == arrIdx )
                            b.appendUndefined( "" );
                        else
                            b.appendAs( fixed[ j ], "" );
                    }
                    keys.insert( b.obj() );
                }
            }
        } else {
            // nonterminal array element to expand, so recurse
            assert( !arrElt.eoo() );
            BSONObjIterator i( arrElt.embeddedObject() );
            while( i.more() ) {
                BSONElement e = i.next();
                if ( e.type() == Object )
                    getKeys( fieldNames, fixed, e.embeddedObject(), keys );
            }
        }
    }

    void getKeysFromObject( const IndexSpec &spec, const BSONObj &obj, BSONObjSetDefaultOrder &keys ) {
        BSONObjIterator i( spec.keys );
        vector< const char * > fieldNames;
        vector< BSONElement > fixed;
        BSONObjBuilder nullKey;
        while( i.more() ) {
            fieldNames.push_back( i.next().fieldName() );
            fixed.push_back( BSONElement() );
            nullKey.appendNull( "" );
        }
        getKeys( fieldNames, fixed, obj, keys );
        if ( keys.empty() )
            keys.insert( nullKey.obj() );
    }

    /* Pull out the relevant key objects from obj, so we
       can index them.  Note that the set is multiple elements
       only when it's a "multikey" array.
       Keys will be left empty if key not found in the object.
    */
    void IndexDetails::getKeysFromObject( const BSONObj& obj, BSONObjSetDefaultOrder& keys) const {
        mongo::getKeysFromObject( IndexSpec( info ) , obj, keys );
    }

    void setDifference(BSONObjSetDefaultOrder &l, BSONObjSetDefaultOrder &r, vector<BSONObj*> &diff) {
        BSONObjSetDefaultOrder::iterator i = l.begin();
        BSONObjSetDefaultOrder::iterator j = r.begin();
        while ( 1 ) {
            if ( i == l.end() )
                break;
            while ( j != r.end() && j->woCompare( *i ) < 0 )
                j++;
            if ( j == r.end() || i->woCompare(*j) != 0  ) {
                const BSONObj *jo = &*i;
                diff.push_back( (BSONObj *) jo );
            }
            i++;
        }
    }

    void getIndexChanges(vector<IndexChanges>& v, NamespaceDetails& d, BSONObj newObj, BSONObj oldObj) { 
        v.resize(d.nIndexes);
        NamespaceDetails::IndexIterator i = d.ii();
        while( i.more() ) {
            int j = i.pos();
            IndexDetails& idx = i.next();
            BSONObj idxKey = idx.info.obj().getObjectField("key"); // eg { ts : 1 }
            IndexChanges& ch = v[j];
            idx.getKeysFromObject(oldObj, ch.oldkeys);
            idx.getKeysFromObject(newObj, ch.newkeys);
            if( ch.newkeys.size() > 1 ) 
                d.setIndexIsMultikey(j);
            setDifference(ch.oldkeys, ch.newkeys, ch.removed);
            setDifference(ch.newkeys, ch.oldkeys, ch.added);
        }
    }

    void dupCheck(vector<IndexChanges>& v, NamespaceDetails& d) {
        NamespaceDetails::IndexIterator i = d.ii();
        while( i.more() ) {
            int j = i.pos();
            v[j].dupCheck(i.next());
        }
    }


}
