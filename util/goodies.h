// goodies.h
// miscellaneous junk

#pragma once

#include "../stdafx.h"

/* set to TRUE if we are exiting */
extern bool goingAway;

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
typedef boost::mutex::scoped_lock lock;

// simple scoped timer
class Timer { 
public:
	Timer() { old = curTimeMicros(); }
	int micros() { 
		unsigned n = curTimeMicros();
		cout << "old:" << old << " new:" << n << endl;
		return tdiff(old, n); 
	}
private:
	unsigned old;
};
