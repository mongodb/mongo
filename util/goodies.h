// goodies.h
// miscellaneous junk

#pragma once

#include "../stdafx.h"

inline void dumpmemory(const char *data, int len) { 
	try {
	const char *q = data;
	const char *p = q;
	while( len > 0 ) {
		for( int i = 0; i < 16; i++ ) { 
			cout << (*p >= 32 && *p <= 126) ? *p : '.';
			p++;
		}
		cout << "  ";
		for( int i = 0; i < 16; i++ )
			cout << (unsigned) *p << ' ';
		cout << endl;
		len -= 16;
	}
	} catch(...) { }
}

