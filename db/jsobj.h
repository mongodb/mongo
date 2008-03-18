// jsobj.h

#pragma once

#include "../stdafx.h"
#include "../util/builder.h"
#include "javajs.h"

#include <set>

class JSObj;
class Record;
class JSObjBuilder;

#pragma pack(push)
#pragma pack(1)

/* BinData = binary data types. 
   EOO = end of object
*/
enum JSType { EOO = 0, Number=1, String=2, Object=3, Array=4, BinData=5, 
              Undefined=6, jstOID=7, Bool=8, Date=9 , jstNULL=10, RegEx=11 ,
              DBRef=12, Code=13, MaxKey=127 };

/* subtypes of BinData.
   bdtCustom and above are ones that the JS compiler understands, but are
   opaque to the database.
*/
enum BinDataType { Function=1, ByteArray=2, bdtCustom=128 };

/*	Object id's are optional for JSObjects.  
	When present they should be the first object member added.
*/
struct OID { 
	long long a;
	unsigned b;
	bool operator==(const OID& r) { return a==r.a&&b==r.b; }
          void out(){ cout << hex << a << hex << b << endl; };
};

/* marshalled js object format:

   <unsigned totalSize> {<byte JSType><cstring FieldName><Data>}* EOO
      totalSize includes itself.

   Data:
     Bool: <byte>
     EOO: nothing follows
     Undefined: nothing follows
     OID: an OID object
     Number: <double>
     String: <unsigned32 strsizewithnull><cstring>
	 Date:   <8bytes>
	 Regex:  <cstring regex><cstring options>
     Object: a nested object, leading with its entire size, which terminates with EOO.
     Array:  same as object
     BinData:
       <int len>
       <byte subtype>
       <byte[len] data>
*/

/* db operation message format 

   unsigned opid;         // arbitary; will be echoed back
   byte operation;
   
   dbInsert:
      int reserved;
      string collection;
      a series of JSObjects terminated with a null object (i.e., just EOO)
   dbUpdate: see query.h
   dbDelete: see query.h
   dbQuery: see query.h
*/

#pragma pack(pop)

/* <type><fieldName    ><value> 
   -------- size() ------------
         -fieldNameSize-
                        value()
   type()
*/
class Element {
	friend class JSElemIter;
	friend class JSObj;
public:
	string toString();
	JSType type() { return (JSType) *data; }
	bool eoo() { return type() == EOO; }
	int size();

	// wrap this element up as a singleton object.
	JSObj wrap();

	const char * fieldName() { 
		if( eoo() ) return ""; // no fieldname for it.
		return data + 1; 
	}

	// raw data be careful:
	const char * value() const { return (data + fieldNameSize + 1); }
	int valuesize() { return size() - fieldNameSize - 1; } 

	bool boolean() { return *value() ? true : false; }

	unsigned long long date() { return *((unsigned long long*) value()); }
	double& number() { return *((double *) value()); }
	OID& oid() { return *((OID*) value()); }

	// for strings
	int valuestrsize() { 
		return *((int *) value());
	}

	// for objects the size *includes* the size of the size field
	int objsize() { 
		return *((int *) value());
	}

	// for strings.  also gives you start of the real data for an embedded object
	const char * valuestr() { return value() + 4; }

	JSObj embeddedObject();

	const char *regex() { assert(type() == RegEx); return value(); }
	const char *regexFlags() { 
		const char *p = regex();
		return p + strlen(p) + 1;
	}

	/* like operator== but doesn't check the fieldname,
	   just the value.
	   */
	bool valuesEqual(Element& r) { 
		bool match= valuesize() == r.valuesize() && 
			memcmp(value(),r.value(),valuesize()) == 0;
		return match;
		// todo: make "0" == 0.0, undefined==null
	}

	bool operator==(Element& r) { 
		if( strcmp(fieldName(), r.fieldName()) != 0 )
			return false;
		return valuesEqual(r);
/*
		int sz = size();
		return sz == r.size() && 
			memcmp(data, r.data, sz) == 0;
*/
	}

	const char * rawdata() { return data; }

	Element();

private:
	Element(const char *d) : data(d) {
		fieldNameSize = eoo() ? 0 : strlen(fieldName()) + 1;
		totalSize = -1;
	}
	const char *data;
	int fieldNameSize;
	int totalSize;
};

class JSObj {
	friend class JSElemIter;
public:
explicit
	JSObj(const char *msgdata, bool ifree = false) : iFree(ifree) {
		_objdata = msgdata;
		_objsize = *((int*) _objdata);
	}
	JSObj(Record *r);
	JSObj() : _objsize(0), _objdata(0), iFree(false) { }

	~JSObj() { if( iFree ) { free((void*)_objdata); _objdata=0; } }

	void iWillFree() { 
		assert(!iFree); iFree = true; 
	}

	string toString() const;
	/* note: addFields always adds _id even if not specified */
	int addFields(JSObj& from, set<string>& fields); /* returns n added */
	int getFieldNames(set<string>& fields);

	Element getField(const char *name); /* return has eoo() true if no match */
	const char * getStringField(const char *name);
	JSObj getObjectField(const char *name);

	/* makes a new JSObj with the fields specified in pattern.
       fields returned in the order they appear in pattern.
	   if any field missing, you get back an empty object overall.
	   */
	JSObj extractFields(JSObj pattern, JSObjBuilder& b);

	const char *objdata() const { return _objdata; }
	int objsize() const { return _objsize; } // includes the embedded size field
	bool isEmpty() const { return objsize() <= 5; }

	/* this is broken if elements aren't in the same order. */
	bool operator<(const JSObj& r) const { return woCompare(r) < 0; }

	/* -1: l<r. 0:l==r. 1:l>r 
	   wo='well ordered'.  fields must be in same order in each object.
	*/
	int woCompare(const JSObj& r) const;
	bool woEqual(const JSObj& r) const { 
		return _objsize==r._objsize && memcmp(_objdata,r._objdata,_objsize)==0;
	}
	bool operator==(const JSObj& r) const { 
		return this->woEqual(r);*this == r; 
	}

	Element firstElement() { 
		return Element(objdata() + 4);
	}
	Element findElement(const char *name);

	OID* getOID() {
		Element e = firstElement();
		if( e.type() != jstOID )
			return 0;
		return &e.oid();
	}

	JSObj(const JSObj& r) { 
		_objsize = r._objsize;
		_objdata = r._objdata;
		iFree = r.iFree;
		if( iFree ) { 
			((JSObj&)r)._objdata = 0; 
			((JSObj&)r).iFree = false; 
		}
	}
	JSObj& operator=(JSObj& r) {
		if( iFree ) free((void*)_objdata); 
		_objsize = r._objsize;
		_objdata = r._objdata;
		iFree = r.iFree;
		/* kind of like auto_ptrs here.  note we leave objsize as it was 
		   so you'll get notified if you try to use the object, instead of just
		   thinking it is empty.
		   */
		if( iFree ) { r._objdata = 0; r.iFree = false; }
		return *this;
	}

	/* makes a copy of the object.  Normally, a jsobj points to data "owned" 
	   by something else.  this is a useful way to get your own copy of the buffer 
	   data (which is freed when the new jsobj destructs).
	   */
	JSObj copy() const;

	int hash() const {
		unsigned x = 0;
		const char *p = _objdata;
		for( int i = 0; i < _objsize; i++ )
			x = x * 131 + p[i];
		return (x & 0x7fffffff) | 0x8000000; // must be > 0
	}

	bool iFree;
private:
	int _objsize;
	const char *_objdata;
};

class JSObjBuilder { 
public:
	JSObjBuilder() { b.skip(4); /*leave room for size field*/ }

	/* add all the fields from the object specified to this object */
	void appendElements(JSObj x);

	void append(Element& e) { b.append((void*) e.rawdata(), e.size()); }

	/* append an element but with a new name */
	void appendAs(Element& e, const char *as) { 
		b.append((char) e.type());
		b.append(as);
		b.append((void *) e.value(), e.valuesize());
	}

	void append(const char *fieldName, double n) { 
		b.append((char) Number);
		b.append(fieldName);
		b.append(n);
	}
	void appendDate(const char *fieldName, unsigned long long dt) { 
		b.append((char) Date);
		b.append(fieldName);
		b.append(dt);
	}
	void append(const char *fieldName, const char *str) {
		b.append((char) String);
		b.append(fieldName);
		b.append((int) strlen(str)+1);
		b.append(str);
	}

	JSObj doneAndDecouple() { 
		int l;
		return JSObj(decouple(l), true);
	}
	JSObj done() { 
		return JSObj(_done());
	}

	/* assume ownership of the buffer - you must then free it (with free()) */
	char* decouple(int& l) {
		char *x = _done();
		l = b.len();
		b.decouple();
		return x;
	}
	void decouple() { b.decouple(); } // post done() call version.  be sure jsobj frees...

private:
	char* _done() { 
		b.append((char) EOO);
		char *data = b.buf();
		*((int*)data) = b.len();
		return data;
	}

	BufBuilder b;
};

class JSElemIter {
public:
	JSElemIter(const JSObj& jso) {
		pos = jso.objdata() + 4;
		theend = jso.objdata() + jso.objsize();
	}
	bool more() { return pos < theend; }
	Element next() {
		Element e(pos);
		pos += e.size();
		return e;
	}
private:
	const char *pos;
	const char *theend;
};

#include <pcrecpp.h> 

class RegexMatcher { 
public:
	const char *fieldName;
	pcrecpp::RE *re;
	RegexMatcher() { re = 0; }
	~RegexMatcher() { delete re; }
};

class Where { 
public:
	~Where() {
		JavaJS.scopeFree(scope);
	}
	jlong scope, func;
};

/* For when a js object is a query pattern. 

   e.g.
       db.foo.find( { a : 3 } );

   { a : 3 } is the pattern object.

   GT/LT:
   { a : { $gt

*/
class JSMatcher { 
public:
	enum { 
		Equality = 0,
		LT = 0x1,
		LTE = 0x3,
		GTE = 0x6,
		GT = 0x4 
	};

	static int opDirection(int op) { 
		return op <= LTE ? -1 : 1;
	}

	JSMatcher(JSObj& pattern);

	~JSMatcher() { 
		for( int i = 0; i < nBuilders; i++ )
			delete builders[i];
		delete where;
	}

	bool matches(JSObj& j, bool *deep = 0);

	int getN() { return n; }

private:
	int valuesMatch(Element& l, Element& r, int op);

	Where *where;
	JSObj& jsobj;
	vector<Element> toMatch;
	vector<int> compareOp;
	int n;

	RegexMatcher regexs[4];
	int nRegex;

	// so we delete the mem when we're done:
	JSObjBuilder *builders[8];
	int nBuilders;
};

extern JSObj maxKey;

/*- just for testing -- */

#pragma pack(push)
#pragma pack(1)
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
};
#pragma pack(pop)
extern JSObj1 js1;

inline JSObj Element::embeddedObject() { 
	assert( type()==Object || type()==Array ); 
	return JSObj(value()); 
}

inline JSObj JSObj::copy() const { 
	if( _objsize == 0 )
		return *this;
	char *p = (char*) malloc(_objsize);
	memcpy(p, _objdata, _objsize);
	return JSObj(p, true);
}

// wrap this element up as a singleton object.
inline JSObj Element::wrap() { 
	JSObjBuilder b;
	b.append(*this);
	return b.doneAndDecouple();
}

inline Element JSObj::findElement(const char *name) { 
	JSElemIter it(*this);
	while( it.more() ) {
		Element e = it.next();
		if( strcmp(name, e.fieldName()) == 0 ) 
			return e;
	}
	return Element();
}

/* add all the fields from the object specified to this object */
inline void JSObjBuilder::appendElements(JSObj x) { 
	JSElemIter it(x);
	while( it.more() ) {
		Element e = it.next();
		if( e.eoo() ) break;
		append(e);
	}
}

