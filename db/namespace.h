// namespace.h

#pragma once

#include "../util/hashtab.h"
#include "../util/mmap.h"

class Cursor;

#pragma pack(push)
#pragma pack(1)

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
	/* index info object. 
	  { name:"nameofindex", ns:"parentnsname", key: {keypattobject} } 
	*/
	DiskLoc info; 

	/* pull out the relevant key objects from obj, so we
       can index them.  Note that the set is multiple elements 
	   only when it's a "multikey" array.
       keys will be left empty if key not found in the object.
	*/
	void getKeysFromObject(JSObj& obj, set<JSObj>& keys);

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
	char reserved[256-16-4-4-8*MaxIndexes-8-8-8];

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

class NamespaceIndex {
	friend class NamespaceCursor;
public:
	NamespaceIndex() { }

	void init(const char *dir, const char *client) { 
		string path = dir;
		path += client;
		path += ".ns";
		const int LEN = 16 * 1024 * 1024;
		void *p = f.map(path.c_str(), LEN);
		if( p == 0 ) { 
			problem() << "couldn't open namespace.idx " << path.c_str() << " terminating" << endl;
			exit(-3);
		}
		ht = new HashTable<Namespace,NamespaceDetails>(p, LEN, "namespace index");
	}

	void add(const char *ns, DiskLoc& loc) { 
		Namespace n(ns);
		NamespaceDetails details;
		details.lastExtent = details.firstExtent = loc;
		ht->put(n, details);
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

/*
class NamespaceIndexMgr { 
public:
	NamespaceIndexMgr() { }
	NamespaceIndex* get(const char *client) { 
		map<string,NamespaceIndex*>::iterator it = m.find(client);
		if( it != m.end() )
			return it->second;
		NamespaceIndex *ni = new NamespaceIndex();
		ni->init(dbpath, client);
		m[client] = ni;
		return ni;
	}
private:
	map<string,NamespaceIndex*> m;
};

extern NamespaceIndexMgr namespaceIndexMgr;
*/

// "client.a.b.c" -> "client"
inline void nsToClient(const char *ns, char *client) { 
	const char *p = ns;
	char *q = client;
	while( *p != '.' ) { 
		if( *p == 0 ) { 
			assert(false);
			*client = 0;
			return;
		}
		*q++ = *p++;
	}
	*q = 0;
	assert(q-client<256);
}

/*
inline NamespaceIndex* nsindex(const char *ns) { 
	char client[256];
	nsToClient(ns, client);
	return namespaceIndexMgr.get(client);
}

inline NamespaceDetails* nsdetails(const char *ns) { 
	return nsindex(ns)->details(ns);
}
*/

//auto_ptr<Cursor> makeNamespaceCursor();
