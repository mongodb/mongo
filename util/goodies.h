// goodies.h
// miscellaneous junk

#pragma once

#include "../stdafx.h"

inline void dumpmemory(const char *data, int len) { 
	if( len > 1024 )
		len = 1024;
	try {
	const char *q = data;
	const char *p = q;
	while( len > 0 ) {
		for( int i = 0; i < 16; i++ ) { 
			if( *p >= 32 && *p <= 126 )
				cout << *p;
			else 
				cout << '.';
			p++;
		}
		cout << "  ";
		p -= 16;
		for( int i = 0; i < 16; i++ )
			cout << (unsigned) ((unsigned char)*p++) << ' ';
		cout << endl;
		len -= 16;
	}
	} catch(...) {
	}
}

#include <boost/thread/thread.hpp>
#include <boost/thread/xtime.hpp>

inline void sleepsecs(int s) { 
	boost::xtime xt;
	boost::xtime_get(&xt, boost::TIME_UTC);
	xt.sec += s;
	boost::thread::sleep(xt);
}
inline void sleepmillis(int s) { 
	boost::xtime xt;
	boost::xtime_get(&xt, boost::TIME_UTC);
	xt.nsec += s * 1000000;
	boost::thread::sleep(xt);
}
