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

extern int otherTraceLevel;

DiskLoc maxDiskLoc(0x7fffffff, 0x7fffffff);
DiskLoc minDiskLoc(0, 1);

BtreeCursor::BtreeCursor(IndexDetails& _id, JSObj& k, int _direction, bool sm) : 
    indexDetails(_id),
    direction(_direction), stopmiss(sm) 
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
	bucket = indexDetails.head.btree()->
		locate(indexDetails.head, k, keyOfs, found, direction > 0 ? minDiskLoc : maxDiskLoc, direction);
	checkUnused();
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

bool BtreeCursor::advance() { 
	if( bucket.isNull() )
		return false;
	bucket = bucket.btree()->advance(bucket, keyOfs, direction, "BtreeCursor::advance");
	checkUnused();
	return !bucket.isNull();
}

void BtreeCursor::noteLocation() {
	if( !eof() ) {
		JSObj o = bucket.btree()->keyAt(keyOfs).copy();
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

		// Note keyAt() returns an empty JSObj if keyOfs is now out of range, 
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
