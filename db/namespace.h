// namespace.h

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

#pragma once

#include "../util/hashtab.h"
#include "../util/mmap.h"

class Cursor;

#pragma pack(push,1)

class Namespace {
public:
	Namespace(const char *ns) { 
		*this = ns;
	}
	Namespace& operator=(const char *ns) { 
		memset(buf, 0, 128); /* this is just to keep stuff clean in the files for easy dumping and reading */
		strcpy_s(buf, 128, ns); return *this; 
	}

	void kill() { 
		buf[0] = 0x7f;
	}

	bool operator==(const char *r) { return strcmp(buf, r) == 0; }
	bool operator==(const Namespace& r) { return strcmp(buf, r.buf) == 0; }
	int hash() const {
		unsigned x = 0;
		const char *p = buf;
		while( *p ) { 
			x = x * 131 + *p;
			p++;
		}
		return (x & 0x7fffffff) | 0x8000000; // must be > 0
	}

	char buf[128];
};

const int Buckets = 19;
const int MaxBucket = 18;
const int MaxIndexes = 10;

class IndexDetails { 
public:
	DiskLoc head; /* btree head */

	/* Location of index info object. Format:

 	    { name:"nameofindex", ns:"parentnsname", key: {keypattobject} } 

	   This object is in the system.indexes collection.  Note that since we 
	   have a pointer to the object here, the object in system.indexes must 
	   never move.
	*/
	DiskLoc info;

	/* pull out the relevant key objects from obj, so we
       can index them.  Note that the set is multiple elements 
	   only when it's a "multikey" array.
       keys will be left empty if key not found in the object.
	*/
	void getKeysFromObject(JSObj& obj, set<JSObj>& keys);

    /* get the key pattern for this object. 
       e.g., { lastname:1, firstname:1 }
    */
    JSObj key() { 
        return info.obj().getObjectField("key");
    }

    // returns name of this index's storage area
	// client.table.$index
	string indexNamespace() { 
		JSObj io = info.obj();
		string s;
		s.reserve(128);
		s = io.getStringField("ns");
		assert( !s.empty() );
		s += ".$";
		s += io.getStringField("name"); 
		return s;
	}

	string indexName() { // e.g. "ts_1"
		JSObj io = info.obj();
		return io.getStringField("name");
	}

	/* gets not our namespace name (indexNamespace for that), 
	   but the collection we index, its name.
	   */
	string parentNS() {
		JSObj io = info.obj();
		return io.getStringField("ns");
	}

	/* delete this index.  does NOT celan up the system catalog
	   (system.indexes or system.namespaces) -- only NamespaceIndex.
	*/
	void kill();
};

extern int bucketSizes[];

/* this is the "header" for a collection that has all its details.  in the .ns file.
*/
class NamespaceDetails {
public:
	NamespaceDetails() { 
		/* be sure to initialize new fields here -- doesn't default to zeroes the way we use it */
		datasize = nrecords = 0;
		lastExtentSize = 0;
		nIndexes = 0;
		capped = 0;
		max = 0x7fffffff;
		paddingFactor = 1.0;
		flags = 0;
		memset(reserved, 0, sizeof(reserved));
	} 
	DiskLoc firstExtent;
	DiskLoc lastExtent;
	DiskLoc deletedList[Buckets];
	long long datasize;
	long long nrecords;
	int lastExtentSize;
	int nIndexes;
	IndexDetails indexes[MaxIndexes];
	int capped;
	int max; // max # of objects for a capped table.
	double paddingFactor; // 1.0 = no padding.
	int flags;
	char reserved[256-16-4-4-8*MaxIndexes-8-8-8-4];

	enum { 
		Flag_HaveIdIndex = 1 // set when we have _id index (ONLY if ensureIdIndex was called -- 0 if that has never been called)
	};

	/* you MUST call when adding an index.  see pdfile.cpp */
	void addingIndex(const char *thisns, IndexDetails& details);

	void aboutToDeleteAnIndex() { flags &= ~Flag_HaveIdIndex; }

	/* returns index of the first index in which the field is present. -1 if not present. */
	int fieldIsIndexed(const char *fieldName);

	void paddingFits() { 
		double x = paddingFactor - 0.01;
		if( x >= 1.0 )
			paddingFactor = x;
	}
	void paddingTooSmall() { 
		double x = paddingFactor + 0.6;
		if( x <= 2.0 )
			paddingFactor = x;
	}

	//returns offset in indexes[]
	int findIndexByName(const char *name) { 
		for( int i = 0; i < nIndexes; i++ ) {
			if( strcmp(indexes[i].info.obj().getStringField("name"),name) == 0 )
				return i;
		}
		return -1;
	}

	/* return which "deleted bucket" for this size object */
	static int bucket(int n) { 
		for( int i = 0; i < Buckets; i++ )
			if( bucketSizes[i] > n )
				return i;
		return Buckets-1;
	}

	/* allocate a new record.  lenToAlloc includes headers. */
	DiskLoc alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc);

	/* add a given record to the deleted chains for this NS */
	void addDeletedRec(DeletedRecord *d, DiskLoc dloc);

	void dumpDeleted(set<DiskLoc> *extents = 0);
private:
	DiskLoc __stdAlloc(int len);
	DiskLoc _alloc(const char *ns, int len);
	void compact();
};

#pragma pack(pop)

/* these are things we know / compute about a namespace that are transient -- things 
   we don't actually store in the .ns file.  so mainly caching of frequently used 
   information.

   CAUTION: Are you maintaining this properly on a collection drop()?  A dropdatabase()?  Be careful.
            The current field "allIndexKeys" may have too many keys in it on such an occurrence;
            as currently used that does not cause anything terrible to happen.
*/
class NamespaceDetailsTransient : boost::noncopyable {
	string ns;
	bool haveIndexKeys;
	set<string> allIndexKeys;
	void computeIndexKeys();
public:
	NamespaceDetailsTransient(const char *_ns) : ns(_ns) { haveIndexKeys=false; /*lazy load them*/ }

	/* get set of index keys for this namespace.  handy to quickly check if a given 
	   field is indexed (Note it might be a seconary component of a compound index.) 
	*/
	set<string>& indexKeys() {
		if( !haveIndexKeys ) { haveIndexKeys=true; computeIndexKeys(); }
		return allIndexKeys;
	}

    void addedIndex() { haveIndexKeys=false; }
private:
	static map<const char *,NamespaceDetailsTransient*> map;
public:
	static NamespaceDetailsTransient& get(const char *ns);
};

/* NamespaceIndex is the ".ns" file you see in the data directory.  It is the "system catalog" 
   if you will: at least the core parts.  (Additional info in system.* collections.)
*/
class NamespaceIndex {
	friend class NamespaceCursor;
public:
	NamespaceIndex() { }

	/* returns true if we created (did not exist) during init() */
	bool init(const char *dir, const char *client) { 
		string path = dir;
		path += client;
		path += ".ns";

		bool created = !boost::filesystem::exists(path); 

		const int LEN = 16 * 1024 * 1024;
		void *p = f.map(path.c_str(), LEN);
		if( p == 0 ) { 
			problem() << "couldn't open namespace.idx " << path.c_str() << " terminating" << endl;
			exit(-3);
		}
		ht = new HashTable<Namespace,NamespaceDetails>(p, LEN, "namespace index");
		return created;
	}

	void add(const char *ns, DiskLoc& loc) { 
		Namespace n(ns);
		NamespaceDetails details;
		details.lastExtent = details.firstExtent = loc;
		ht->put(n, details);
	}

	/* just for diagnostics */
	size_t detailsOffset(NamespaceDetails *d) { 
	  return ((char *) d) -  (char *) ht->nodes;
	}

	NamespaceDetails* details(const char *ns) { 
		Namespace n(ns);
		return ht->get(n); 
	}

	void kill(const char *ns) {
		Namespace n(ns);
		ht->kill(n); 
	}

	bool find(const char *ns, DiskLoc& loc) { 
		NamespaceDetails *l = details(ns);
		if( l ) {
			loc = l->firstExtent;
			return true;
		}
		return false;
	}

private:
	MemoryMappedFile f;
	HashTable<Namespace,NamespaceDetails> *ht;
};

extern const char *dbpath;

// "client.a.b.c" -> "client"
const int MaxClientLen = 256;
inline void nsToClient(const char *ns, char *client) { 
	const char *p = ns;
	char *q = client;
	while( *p != '.' ) { 
		if( *p == 0 ) 
            break;
		*q++ = *p++;
	}
	*q = 0;
	if(q-client>=MaxClientLen) {
		problem() << "nsToClient: ns too long. terminating, buf overrun condition" << endl;
		dbexit(60);
	}
}
inline string nsToClient(const char *ns) {
    char buf[MaxClientLen];
    nsToClient(ns, buf);
    return buf;
}
