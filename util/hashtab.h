/* hashtab.h

   Simple, fixed size hash table.  Darn simple.

   Uses a contiguous block of memory, so you can put it in a memory mapped file very easily.
*/

#include "../stdafx.h"
#include <map>

#pragma pack(push)
#pragma pack(1)

/* you should define:

   int Key::hash() return > 0 always.
*/

template <
	class Key,
	class Type
>
class HashTable {
public:
	const char *name;
	struct Node {
		int hash;
		Key k;
		Type value;
		bool inUse() { return hash != 0; }
	} *nodes;
	int n;

	int _find(const Key& k, bool& found) {
		found = false;
		int h = k.hash();
		int i = h % n;
		int start = i;
		int chain = 0;
		while( 1 ) {
			if( !nodes[i].inUse() ) {
				return i;
			}
			if( nodes[i].hash == h && nodes[i].k == k ) {
				found = true;
				return i;
			}
			chain++;
			i = (i+1) % n;
			if( i == start ) { 
				cout << "warning: hashtable is full " << name << endl;
				return -1;
			}
			if( chain == 200 )
				cout << "warning: hashtable long chain " << name << endl;
		}
	}

public:
	/* buf must be all zeroes on initialization. */
	HashTable(void *buf, int buflen, const char *_name) : name(_name) { 
		int m = sizeof(Node);
		n = buflen / m;
		if( (n & 1) == 0 )
			n--;
		nodes = (Node *) buf;
		assert(nodes[n-1].hash == 0);
		assert(nodes[0].hash == 0);
	}

	Type* get(const Key& k) { 
		bool found;
		int i = _find(k, found);
		if( found )
			return &nodes[i].value;
		return 0;
	}

	void put(const Key& k, const Type& value) {
		bool found;
		int i = _find(k, found);
		if( i < 0 )
			return;
		if( !found ) {
			nodes[i].k = k;
			nodes[i].hash = k.hash();
		}
		else {
			assert( nodes[i].hash == k.hash() );
		}
		nodes[i].value = value;
	}

};

#pragma pack(pop)

