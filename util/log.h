// log.h

#pragma once

class Nullstream { 
public:
	Nullstream& operator<<(const char *) { return *this; }
	Nullstream& operator<<(int) { return *this; }
	Nullstream& operator<<(unsigned long) { return *this; }
	Nullstream& operator<<(unsigned) { return *this; }
	Nullstream& operator<<(double) { return *this; }
	Nullstream& operator<<(void *) { return *this; }
	Nullstream& operator<<(long long) { return *this; }
	Nullstream& operator<<(unsigned long long) { return *this; }
	Nullstream& operator<<(const string&) { return *this; }
	Nullstream& operator<< (ostream& ( *endl )(ostream&)) { return *this; }
	Nullstream& operator<< (ios_base& (*hex)(ios_base&)) { return *this; }
};

inline Nullstream& endl ( Nullstream& os ) { }

extern Nullstream nullstream;

//#define cout nullstream

// not threadsafe
inline ostream& problem() {
	ostream& problems = cout;
	time_t t;
	time(&t);
	string now(ctime(&t),0,20);
	problems << "~ " << now;
	if( client ) 
		problems << curNs << ' ';
	return problems;
}

