// btreecursor.cpp

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
#include "btree.h"
#include "pdfile.h"
#include "jsobj.h"

extern int otherTraceLevel;

DiskLoc maxDiskLoc(0x7fffffff, 0x7fffffff);
DiskLoc minDiskLoc(0, 1);

BtreeCursor::BtreeCursor(IndexDetails& _id, const BSONObj& k, int _direction, BSONObj& _query) : 
//    query(_query),
    indexDetails(_id),
    direction(_direction)
{
//otherTraceLevel = 999;

	bool found;
	if( otherTraceLevel >= 12 ) {
		if( otherTraceLevel >= 200 ) { 
			cout << "::BtreeCursor() qtl>200.  validating entire index." << endl;
			indexDetails.head.btree()->fullValidate(indexDetails.head);
		}
		else {
			cout << "BTreeCursor(). dumping head bucket" << endl;
			indexDetails.head.btree()->dump();
		}
	}
	
	findExtremeKeys( _query );
	if( !k.isEmpty() )
		startKey = k;
	
	bucket = indexDetails.head.btree()->
		locate(indexDetails.head, startKey, keyOfs, found, direction > 0 ? minDiskLoc : maxDiskLoc, direction);
	checkUnused();
}

// Given a query, find the lowest and highest keys along our index that could
// potentially match the query.  These lowest and highest keys will be mapped
// to startKey and endKey based on the value of direction.
void BtreeCursor::findExtremeKeys( const BSONObj &query ) {	
	BSONObjBuilder startBuilder;
	BSONObjBuilder endBuilder;
	set< string >fields;
	getFields( indexDetails.keyPattern(), fields );
	for( set<string>::iterator i = fields.begin(); i != fields.end(); ++i ) {
		const char * field = i->c_str();
		BSONElement k = indexDetails.keyPattern().getFieldDotted( field );
		int number = 1;
		if( k.type() == NumberDouble || k.type() == NumberInt )
			number = k.number();
		bool forward = ( ( number > 0 ? 1 : -1 ) * direction > 0 );
		BSONElement startElement;
		BSONElement endElement;
		BSONElement e = query.getFieldDotted( field );
		if ( !e.eoo() && e.type() != RegEx ) {
			int op = getGtLtOp( e );
			bool someLt = ( op == JSMatcher::LT || op == JSMatcher::LTE );
			bool someGt = ( op == JSMatcher::GT || op == JSMatcher::GTE );
			if ( op == JSMatcher::Equality )
				startElement = endElement =	e;
			else if ( ( someLt && forward ) ||
					 ( someGt && !forward ) )
				endElement = e.embeddedObject().firstElement();
			else if ( ( someLt && !forward ) ||
					 ( someGt && forward ) )
				startElement = e.embeddedObject().firstElement();
		}
		appendKeyElement( startBuilder, startElement, field, forward );
		appendKeyElement( endBuilder, endElement, field, !forward );
	}
	startKey = startBuilder.doneAndDecouple();
	endKey = endBuilder.doneAndDecouple();
}

// Expand all field names in key to use dotted notation.
void BtreeCursor::getFields( const BSONObj &key, set< string > &fields ) {
	BSONObjIterator i( key );
	while( 1 ) {
		BSONElement k = i.next();
		if( k.eoo() )
			break;
		bool addedSubfield = false;
		if( k.type() == Object ) {
			set< string > subFields;
			getFields( k.embeddedObject(), subFields );
			for( set< string >::iterator i = subFields.begin(); i != subFields.end(); ++i ) {
				addedSubfield = true;
				fields.insert( k.fieldName() + string( "." ) + *i );
			}
		}
		if ( !addedSubfield )
			fields.insert( k.fieldName() );
	}
}

// If element is non-null, append it to builder; otherwise append min or max.
void BtreeCursor::appendKeyElement( BSONObjBuilder &builder,
								   const BSONElement &element,
								   const char *fieldName,
								   bool defaultMin ) {
	if( !( element == BSONElement() ) )
		builder.appendAs( element, fieldName );
	else
		if( defaultMin )
			builder.appendMinKey( fieldName );
		else
			builder.appendMaxKey( fieldName );
}

/* skip unused keys. */
void BtreeCursor::checkUnused() {
	int u = 0;
	while( 1 ) {
		if( !ok() ) 
			break;
		BtreeBucket *b = bucket.btree();
		_KeyNode& kn = b->k(keyOfs);
		if( kn.isUsed() )
			break;
		bucket = b->advance(bucket, keyOfs, direction, "checkUnused");
		u++;
	}
	if( u > 10 )
		OCCASIONALLY log() << "btree unused skipped:" << u << '\n';
}

// Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
int sgn( int i ) {
	if( i == 0 )
		return 0;
	return i > 0 ? 1 : -1;
}

// Check if the current key is beyond endKey.
void BtreeCursor::checkEnd() {
	if ( bucket.isNull() )
		return;	
	int res = endKey.woCompare( currKey() );
	int cmp = sgn( endKey.woCompare( currKey() ) );
	if ( cmp != 0 && cmp != direction )
		bucket = DiskLoc();
}

bool BtreeCursor::advance() { 
	if( bucket.isNull() )
		return false;
	bucket = bucket.btree()->advance(bucket, keyOfs, direction, "BtreeCursor::advance");
	checkUnused();
	checkEnd();
	return !bucket.isNull();
}

void BtreeCursor::noteLocation() {
	if( !eof() ) {
		BSONObj o = bucket.btree()->keyAt(keyOfs).copy();
		keyAtKeyOfs = o;
		locAtKeyOfs = bucket.btree()->k(keyOfs).recordLoc;
	}
}

/* Since the last noteLocation(), our key may have moved around, and that old cached 
   information may thus be stale and wrong (although often it is right).  We check 
   that here; if we have moved, we have to search back for where we were at.

   i.e., after operations on the index, the BtreeCursor's cached location info may 
   be invalid.  This function ensures validity, so you should call it before using 
   the cursor if other writers have used the database since the last noteLocation
   call.
*/
void BtreeCursor::checkLocation() { 
	if( eof() ) 
		return;

	if( keyOfs >= 0 ) {
		BtreeBucket *b = bucket.btree(); 

		assert( !keyAtKeyOfs.isEmpty() ); 

		// Note keyAt() returns an empty BSONObj if keyOfs is now out of range, 
		// which is possible as keys may have been deleted.
		if( b->keyAt(keyOfs).woEqual(keyAtKeyOfs) &&
			b->k(keyOfs).recordLoc == locAtKeyOfs ) { 
				if( !b->k(keyOfs).isUsed() ) {
					/* we were deleted but still exist as an unused 
					   marker key. advance. 
					*/
					checkUnused();
				}
				return;
		}
	}

	/* normally we don't get to here.  when we do, old position is no longer
 	   valid and we must refind where we left off (which is expensive)
	*/

	bool found;

	/* TODO: Switch to keep indexdetails and do idx.head! */
	bucket = indexDetails.head.btree()->locate(indexDetails.head, keyAtKeyOfs, keyOfs, found, locAtKeyOfs, direction);
	RARELY log() << "  key seems to have moved in the index, refinding. found:" << found << endl;
	if( found )
		checkUnused();
}

/* ----------------------------------------------------------------------------- */

struct BtreeUnitTest { 
	BtreeUnitTest() { 
		assert( minDiskLoc.compare(maxDiskLoc) < 0 );
	}
} btut;
