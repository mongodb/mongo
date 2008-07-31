// goodies.h
// miscellaneous junk

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

#include "../stdafx.h"

#if !defined(_WIN32)
#include <pthread.h>
inline pthread_t GetCurrentThreadId() { return pthread_self(); }
#include <execinfo.h>
/* use "addr2line -CFe <exe>" to parse. */
inline void printStackTrace() {
	void *b[12];
	size_t size;
	char **strings;
	size_t i;
     
	size = backtrace(b, 12);
	strings = backtrace_symbols(b, size);
     
	for (i = 0; i < size; i++)
		cout << ' ' << hex << b[i] << '\n';
	for (i = 0; i < size; i++)
		cout << ' ' << strings[i] << '\n';

	free (strings);
}
#else
inline void printStackTrace() { }
#endif

/* set to TRUE if we are exiting */
extern bool goingAway;

/* find the multimap member which matches a particular key and value.

   note this can be slow if there are a lot with the same key.
*/
template<class C,class K,class V> inline typename C::iterator kv_find(C& c, const K& k,const V& v) { 
	pair<typename C::iterator,typename C::iterator> p = c.equal_range(k);

	for( typename C::iterator it=p.first; it!=p.second; ++it)
		if( it->second == v ) 
			return it;

	return c.end();
} 

bool isPrime(int n); 
int nextPrime(int n);

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

#undef yassert
#include <boost/thread/thread.hpp>
#include <boost/thread/xtime.hpp>
#undef assert
#define assert xassert
#define yassert 1

struct WrappingInt {
	WrappingInt() { x = 0; } 
	WrappingInt(unsigned z) : x(z) { }
	unsigned x;
	operator unsigned() const { return x; }
	WrappingInt& operator++() { x++; return *this; }
	static int diff(unsigned a, unsigned b) { return a-b; }
	bool operator<=(WrappingInt r) { 
		// platform dependent
		int df = (r.x - x);
		return df >= 0;
	}
	bool operator>(WrappingInt r) { return !(r<=*this); }
};

#include <ctime>

inline void time_t_to_String(time_t t, char *buf) {
#if defined(_WIN32)
	ctime_s(buf, 64, &t);
#else
	ctime_r(&t, buf);
#endif
	buf[24] = 0; // don't want the \n
}

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
// note this wraps
inline int tdiff(unsigned told, unsigned tnew) { 
	return WrappingInt::diff(tnew, told);
}
inline unsigned curTimeMillis() {
	boost::xtime xt;
	boost::xtime_get(&xt, boost::TIME_UTC);
	unsigned t = xt.nsec / 1000000;
	return (xt.sec & 0xfffff) * 1000 + t;
}
inline unsigned long long jsTime() {
	boost::xtime xt;
	boost::xtime_get(&xt, boost::TIME_UTC);
	unsigned long long t = xt.nsec / 1000000;
	return ((unsigned long long) xt.sec * 1000) + t;
}
// measures up to 1024 seconds.  or, 512 seconds with tdiff that is...
inline unsigned curTimeMicros() {
	boost::xtime xt;
	boost::xtime_get(&xt, boost::TIME_UTC);
	unsigned t = xt.nsec / 1000;
	unsigned secs = xt.sec % 1024;
	t = secs*1000000 + t;
	return t;
}
using namespace boost;
typedef boost::mutex::scoped_lock boostlock;

// simple scoped timer
class Timer { 
public:
	Timer() { old = curTimeMicros(); }
	int millis() { return micros() / 1000; }
	int micros() { 
		unsigned n = curTimeMicros();
		return tdiff(old, n); 
	}
private:
	unsigned old;
};

/*

class DebugMutex : boost::noncopyable { 
	friend class lock;
	boost::mutex m;
	int locked;
public:
	DebugMutex() : locked(0); { }
	bool isLocked() { return locked; }
};

*/
/*
struct lock { 
boostlock bl;
	DebugMutex& m;
	lock(DebugMutex& _m) : m(_m) {
		do_lock();
	}
	~lock() { do_unlock(); }
}
*/
typedef boostlock lock;

