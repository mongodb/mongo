// log.h

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

#define LOGIT { lock lk(mutex); cout << x; return *this; }
class Logstream {
	static boost::mutex mutex;
public:
	Logstream& operator<<(const char *x) LOGIT
	Logstream& operator<<(char x) LOGIT
	Logstream& operator<<(int x) LOGIT
	Logstream& operator<<(unsigned long x) LOGIT
	Logstream& operator<<(unsigned x) LOGIT
	Logstream& operator<<(double x) LOGIT
	Logstream& operator<<(void *x) LOGIT
	Logstream& operator<<(long long x) LOGIT
	Logstream& operator<<(unsigned long long x) LOGIT
	Logstream& operator<<(const string& x) LOGIT
	Logstream& operator<< (ostream& ( *_endl )(ostream&)) { lock lk(mutex); cout << _endl; return *this; }
	Logstream& operator<< (ios_base& (*_hex)(ios_base&)) { lock lk(mutex); cout << _hex; return *this; }
	Logstream& prolog(bool withNs = false) {
		lock lk(mutex);
		time_t t;
		time(&t);
		string now(ctime(&t),0,20);
		cout << now;
		if( withNs && client ) 
			cout << curNs << ' ';
		return *this;
	}
};
inline Logstream& endl ( Logstream& os ) { return os; }
extern Logstream logstream;

inline Logstream& problem() { return logstream.prolog(true); }
inline Logstream& log() { return logstream.prolog(); }

#define cout logstream
