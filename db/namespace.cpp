// namespacedetails.cpp

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
#include "pdfile.h"
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "objwrappers.h"
#include "btree.h"
#include <algorithm>
#include <list>
#include "query.h"

/* deleted lists -- linked lists of deleted records -- tehy are placed in 'buckets' of various sizes 
   so you can look for a deleterecord about the right size.
*/
int bucketSizes[] = { 
	32, 64, 128, 256, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000,
	0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
	0x400000, 0x800000
};

//NamespaceIndexMgr namespaceIndexMgr;

void NamespaceDetails::addDeletedRec(DeletedRecord *d, DiskLoc dloc) { 
	{ 
		// defensive code: try to make us notice if we reference a deleted record 
		(unsigned&) (((Record *) d)->data) = 0xeeeeeeee;
	}

	dassert( dloc.drec() == d );
	DEBUGGING cout << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs << endl;
	int b = bucket(d->lengthWithHeaders);
	DiskLoc& list = deletedList[b];
	DiskLoc oldHead = list;
	list = dloc;
	d->nextDeleted = oldHead;
}

/* 
   lenToAlloc is WITH header 
*/
DiskLoc NamespaceDetails::alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc) {
	lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
	DiskLoc loc = _alloc(ns, lenToAlloc);
	if( loc.isNull() )
		return loc;

	DeletedRecord *r = loc.drec();

	/* note we want to grab from the front so our next pointers on disk tend
	to go in a forward direction which is important for performance. */
	int regionlen = r->lengthWithHeaders;
	extentLoc.set(loc.a(), r->extentOfs);
	assert( r->extentOfs < loc.getOfs() );

	DEBUGGING cout << "TEMP: alloc() returns " << loc.toString() << ' ' << ns << " lentoalloc:" << lenToAlloc << " ext:" << extentLoc.toString() << endl;

	int left = regionlen - lenToAlloc;
	if( left < 24 || (left < (lenToAlloc >> 3) && capped == 0) ) {
		// you get the whole thing.
		return loc;
	}

	/* split off some for further use. */
	r->lengthWithHeaders = lenToAlloc;
	DiskLoc newDelLoc = loc;
	newDelLoc.inc(lenToAlloc);
	DeletedRecord *newDel = newDelLoc.drec();
	newDel->extentOfs = r->extentOfs;
	newDel->lengthWithHeaders = left;
	newDel->nextDeleted.Null();

	addDeletedRec(newDel, newDelLoc);

	return loc;
}

/* for non-capped collections.
   returned item is out of the deleted list upon return 
*/
DiskLoc NamespaceDetails::__stdAlloc(int len) {
	DiskLoc *prev;
	DiskLoc *bestprev = 0;
	DiskLoc bestmatch;
	int bestmatchlen = 0x7fffffff;
	int b = bucket(len);
	DiskLoc cur = deletedList[b]; prev = &deletedList[b];
	int extra = 5; // look for a better fit, a little.
	int chain = 0;
	while( 1 ) { 
		{
			int a = cur.a();
			if( a < -1 || a >= 100000 ) {
				problem() << "~~ Assertion - cur out of range in _alloc() " << cur.toString() <<
					" b:" << b << " chain:" << chain << '\n';
				sayDbContext();
				if( cur == *prev )
					prev->Null();
				cur.Null();
			}
		}
		if( cur.isNull() ) { 
			// move to next bucket.  if we were doing "extra", just break
			if( bestmatchlen < 0x7fffffff )
				break;
			b++;
			if( b > MaxBucket ) {
				// out of space. alloc a new extent.
				return DiskLoc();
			}
			cur = deletedList[b]; prev = &deletedList[b];
			continue;
		}
		DeletedRecord *r = cur.drec();
		if( r->lengthWithHeaders >= len && 
			r->lengthWithHeaders < bestmatchlen ) {
				bestmatchlen = r->lengthWithHeaders;
				bestmatch = cur;
				bestprev = prev;
		}
		if( bestmatchlen < 0x7fffffff && --extra <= 0 )
			break;
		if( ++chain > 30 && b < MaxBucket ) {
			// too slow, force move to next bucket to grab a big chunk
			//b++;
			chain = 0;
			cur.Null();
		}
		else {
			if( r->nextDeleted.getOfs() == 0 ) {
				problem() << "~~ Assertion - bad nextDeleted " << r->nextDeleted.toString() << 
					" b:" << b << " chain:" << chain << ", fixing.\n";
				r->nextDeleted.Null();
			}
			cur = r->nextDeleted; prev = &r->nextDeleted;
		}
	}

	/* unlink ourself from the deleted list */
	{
		DeletedRecord *bmr = bestmatch.drec();
		*bestprev = bmr->nextDeleted;
		bmr->nextDeleted.setInvalid(); // defensive.
		assert(bmr->extentOfs < bestmatch.getOfs());
	}

	return bestmatch;
}

void NamespaceDetails::dumpDeleted(set<DiskLoc> *extents) { 
//	cout << "DUMP deleted chains" << endl;
	for( int i = 0; i < Buckets; i++ ) { 
//		cout << "  bucket " << i << endl;
		DiskLoc dl = deletedList[i];
		while( !dl.isNull() ) { 
			DeletedRecord *r = dl.drec();
			DiskLoc extLoc(dl.a(), r->extentOfs);
			if( extents == 0 || extents->count(extLoc) <= 0 ) {
				cout << "  bucket " << i << endl;
				cout << "   " << dl.toString() << " ext:" << extLoc.toString();
				if( extents && extents->count(extLoc) <= 0 )
					cout << '?';
				cout << " len:" << r->lengthWithHeaders << endl;
			}
			dl = r->nextDeleted;
		}
	}
//	cout << endl;
}

/* combine adjacent deleted records

   this is O(n^2) but we call it for capped tables where typically n==1 or 2! 
   (or 3...there will be a little unused sliver at the end of the extent.)
*/
void NamespaceDetails::compact() { 
	assert(capped);
	list<DiskLoc> drecs;

	for( int i = 0; i < Buckets; i++ ) { 
		DiskLoc dl = deletedList[i];
		deletedList[i].Null();
		while( !dl.isNull() ) { 
			DeletedRecord *r = dl.drec();
			drecs.push_back(dl);
			dl = r->nextDeleted;
		}
	}

	drecs.sort();

	list<DiskLoc>::iterator j = drecs.begin(); 
	assert( j != drecs.end() );
	DiskLoc a = *j;
	while( 1 ) {
		j++;
		if( j == drecs.end() ) {
			DEBUGGING cout << "TEMP: compact adddelrec\n";
			addDeletedRec(a.drec(), a);
			break;
		}
		DiskLoc b = *j;
		while( a.a() == b.a() && a.getOfs() + a.drec()->lengthWithHeaders == b.getOfs() ) { 
			// a & b are adjacent.  merge.
			a.drec()->lengthWithHeaders += b.drec()->lengthWithHeaders;
			j++;
			if( j == drecs.end() ) {
				DEBUGGING cout << "temp: compact adddelrec2\n";
				addDeletedRec(a.drec(), a);
				return;
			}
			b = *j;
		}
		DEBUGGING cout << "temp: compact adddelrec3\n";
		addDeletedRec(a.drec(), a);
		a = b;
	}
}

/* alloc with capped table handling. */
int n_complaints_cap = 0;
DiskLoc NamespaceDetails::_alloc(const char *ns, int len) {
	if( !capped )
		return __stdAlloc(len);

	// capped. 

	assert( len < 400000000 );
	int passes = 0;
	DiskLoc loc;

	// delete records until we have room and the max # objects limit achieved.
	Extent *theExtent = firstExtent.ext(); // only one extent if capped.
	dassert( theExtent->ns == ns ); 
	theExtent->assertOk();
	while( 1 ) {
		if( nrecords < max ) { 
			loc = __stdAlloc(len);
			if( !loc.isNull() )
				break;
		}

		DiskLoc fr = theExtent->firstRecord;
		if( fr.isNull() ) { 
			if( ++n_complaints_cap < 8 ) {
				cout << "couldn't make room for new record in capped ns " << ns << '\n'
					<< "  len: " << len << " extentsize:" << lastExtentSize << '\n';
				cout << "  magic: " << hex << theExtent->magic << " extent->ns: " << theExtent->ns.buf << '\n';
				cout << "  fr: " << theExtent->firstRecord.toString() << 
					" lr: " << theExtent->lastRecord.toString() << " extent->len: " << theExtent->length << '\n';
				assert( len * 5 > lastExtentSize ); // assume it is unusually large record; if not, something is broken
			}
			return DiskLoc();
		}

		theDataFileMgr.deleteRecord(ns, fr.rec(), fr, true);
		compact();
		assert( ++passes < 5000 );
	}

	return loc;
}

/* you MUST call when adding an index.  see pdfile.cpp */
void NamespaceDetails::addingIndex(const char *thisns, IndexDetails& details) { 
	assert( nsdetails(thisns) == this );
	assert( &details == &indexes[nIndexes] );
	nIndexes++;
	NamespaceDetailsTransient::get(thisns).addedIndex();
}

/* returns index of the first index in which the field is present. -1 if not present. 
   (aug08 - this method not currently used)
*/
int NamespaceDetails::fieldIsIndexed(const char *fieldName) {
	for( int i = 0; i < nIndexes; i++ ) {
		IndexDetails& idx = indexes[i];
		JSObj idxKey = idx.info.obj().getObjectField("key"); // e.g., { ts : -1 }
		if( !idxKey.findElement(fieldName).eoo() )
			return i;
	}
	return -1;
}

/* ------------------------------------------------------------------------- */

map<const char *,NamespaceDetailsTransient*> NamespaceDetailsTransient::map;
typedef map<const char *,NamespaceDetailsTransient*>::iterator ouriter;

NamespaceDetailsTransient& NamespaceDetailsTransient::get(const char *ns) { 
	NamespaceDetailsTransient*& t = map[ns];
	if( t == 0 )
		t = new NamespaceDetailsTransient(ns);
	return *t;
}

void NamespaceDetailsTransient::computeIndexKeys() {
	NamespaceDetails *d = nsdetails(ns.c_str());
    for( int i = 0; i < d->nIndexes; i++ ) {
//        set<string> fields;
        d->indexes[i].key().getFieldNames(allIndexKeys);
//        allIndexKeys.insert(fields.begin(),fields.end());
    }
}

/* ------------------------------------------------------------------------- */

/* add a new namespace to the system catalog (<dbname>.system.namespaces).
   options: { capped : ..., size : ... }
*/
void addNewNamespaceToCatalog(const char *ns, JSObj *options = 0) {
	log() << "New namespace: " << ns << endl;
	if( strstr(ns, "system.namespaces") ) { 
		// system.namespaces holds all the others, so it is not explicitly listed in the catalog.
		// TODO: fix above should not be strstr!
		return;
	}

	{
		JSObjBuilder b;
		b.append("name", ns);
		if( options )
			b.append("options", *options);
		JSObj j = b.done();
		char client[256];
		nsToClient(ns, client);
		string s = client;
		s += ".system.namespaces";
		theDataFileMgr.insert(s.c_str(), j.objdata(), j.objsize(), true);
	}
}

