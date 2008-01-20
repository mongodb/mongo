// lru-ish map.h

#pragma once

#include "../stdafx.h"
#include "../util/goodies.h"

/* Your K object must define:
	 int hash() - must always return > 0.
	 operator==
*/

template <class K, class V, int MaxChain>
class LRUishMap { 
public:
	LRUishMap(int _n) { 
		n = nextPrime(_n);
		keys = new K[n];
		hashes = new int[n];
		for( int i = 0; i < n; i++ ) hashes[i] = 0;
	}
	~LRUishMap() {
		delete[] keys;
		delete[] hashes;
	}

	int _find(const K& k, bool& found) {
		int h = k.hash(); 
		assert( h > 0 );
		int j = h % n;
		int first = j;
		for( int i = 0; i < MaxChain; i++ ) { 
			if( hashes[j] == h ) { 
				if( keys[j] == k ) { 
					found = true;
					return j;
				}
			}
			else if( hashes[j] == 0 ) { 
				found = false;
				return j;
			}
		}
		found = false;
		return first;
	}

	V* find(const K& k) { 
		bool found;
		int j = _find(k, found);
		return found ? &values[j] : 0;
	}

private:
	int n;
	K *keys;
	int *hashes;
	V *values;
};
