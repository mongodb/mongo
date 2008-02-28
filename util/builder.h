/* builder.h

*/

#pragma once

#include "../stdafx.h"

class BufBuilder {
public:
	BufBuilder(int initsize = 512) : size(initsize) { 
		data = (char *) malloc(size); assert(data);
		l = 0;
	}
	~BufBuilder() { kill(); } 

	void kill() {
		if( data ) {
			free(data);
			data = 0;
		}
	}

	/* leave room for some stuff later */
	void skip(int n) { grow(n); }

	/* note this may be deallocated (realloced) if you keep writing. */
	char* buf() { return data; }

	/* assume ownership of the buffer - you must then free it */
	void decouple() { data = 0;	}

	template<class T> void append(T j) { *((T*)grow(sizeof(T))) = j; }
	void append(short j) { append<short>(j); }
	void append(int j) { append<int>(j); }
	void append(unsigned j) { append<unsigned>(j); }
	void append(bool j) { append<bool>(j); }
	void append(double j) { append<double>(j); }

	void append(void *src, int len) { memcpy(grow(len), src, len); }

	void append(const char *str) { 
		append((void*) str, strlen(str)+1);
	}

	int len() { return l; }

private:
	/* returns the pre-grow write position */
	char* grow(int by) {
		int oldlen = l;
		l += by;
		if( l > size ) {
			int a = size * 2;
			if( l > a )
				a = l + 16 * 1024;
			assert( a < 64 * 1024 * 1024 );
			data = (char *) realloc(data, a);
			size= a;
		}
		return data + oldlen;
	}

	char *data;
	int l;
	int size;
};
