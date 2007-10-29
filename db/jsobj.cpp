// jsobj.cpp

#include "stdafx.h"
#include "jsobj.h"

int Element::size() {
	if( totalSize >= 0 )
		return totalSize;

	int x = 1;
	switch( type() ) {
		case EOO:
		case Undefined:
		case jstNULL:
			break;
		case Bool:
			x = 2;
			break;
		case Date:
		case Number:
			x = 9;
			break;
		case jstOID:
			x = 13;
			break;
		case String:
		case Object:
		case Array:
			x = valuestrsize() + 4 + 1;
			break;
		case BinData:
			x = valuestrsize() + 4 + 1 + 1/*subtype*/;
			break;
		case RegEx:
			{
				const char *p = value();
				int len1 = strlen(p);
				p = p + len1 + 1;
				x = 1 + len1 + strlen(p);
			}
			break;
		default:
			assert(false);
			cout << "Element: bad type " << (int) type() << endl;
	}
	totalSize =  x + fieldNameSize;
	return totalSize;
}

JSMatcher::JSMatcher(JSObj &_jsobj) : 
   jsobj(_jsobj) 
{
	JSElemIter i(jsobj);
	n = 0;
	while( i.more() ) {
		Element e = i.next();
		if( !e.eoo() ) {
			toMatch.push_back(e);
			n++;
		}
	}
}

bool JSMatcher::matches(JSObj& jsobj) {
	/* assuming there is usually only one thing to match.  if more this
	could be slow sometimes. */

	for( int i = 0; i < n; i++ ) {
		Element& m = toMatch[i];
		JSElemIter k(jsobj);
		while( k.more() ) {
			if( k.next() == m )
				goto ok;
		}
		return false;
ok:
		;
	}

	return true;
}

//------------------------------------------------------------------

#pragma pack(push)
#pragma pack(1)

struct JSObj0 {
	JSObj0() { totsize = 5; eoo = EOO; }
	int totsize;
	char eoo;
} js0;

struct JSObj1 {
	JSObj1() {
		totsize=sizeof(JSObj1); 
		n = Number; strcpy_s(nname, 5, "abcd"); N = 3.1;
		s = String; strcpy_s(sname, 7, "abcdef"); slen = 10; 
		strcpy_s(sval, 10, "123456789"); eoo = EOO;
	}
	unsigned totsize;

	char n;
	char nname[5];
	double N;

	char s;
	char sname[7];
	unsigned slen;
	char sval[10];

	char eoo;
} js1;

struct JSObj2 {
	JSObj2() {
		totsize=sizeof(JSObj2);
		s = String; strcpy_s(sname, 7, "abcdef"); slen = 10; 
		strcpy_s(sval, 10, "123456789"); eoo = EOO;
	}
	unsigned totsize;
	char s;
	char sname[7];
	unsigned slen;
	char sval[10];
	char eoo;
} js2;

struct JSUnitTest {
	JSUnitTest() {
		JSObj j1((const char *) &js1);
		JSObj j2((const char *) &js2);
		JSMatcher m(j2);
		assert( m.matches(j1) );
		js2.sval[0] = 'z';
		assert( !m.matches(j1) );
		JSMatcher n(j1);
		assert( n.matches(j1) );
		assert( !n.matches(j2) );

		JSObj j0((const char *) &js0);
		JSMatcher p(j0);
		assert( p.matches(j1) );
		assert( p.matches(j2) );
	}
} jsunittest;

#pragma pack(pop)
