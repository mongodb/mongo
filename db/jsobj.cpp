// jsobj.cpp

#include "stdafx.h"
#include "jsobj.h"
#include "../util/goodies.h"
#include "javajs.h"

class Where { 
public:
	~Where() {
		JavaJS->scopeFree(scope);
		scope = 0; func = 0;
	}
	jlong scope, func;
};

JSMatcher::~JSMatcher() { 
	for( int i = 0; i < nBuilders; i++ )
		delete builders[i];
	delete where;
}

Element nullElement;

string Element::toString() { 
	stringstream s;
	switch( type() ) {
    case EOO:
		return "EOO";
    case Date:
		s << fieldName() << ": Date(" << hex << date() << ')'; break;
	case Number:
		s << fieldName() << ": " << number(); break;
	case Bool: 
		s << fieldName() << ": " << boolean() ? "true" : "false"; break;
	case Object:
	case Array:
		s << fieldName() << ": " << embeddedObject().toString(); break;
	case Undefined:
		s << fieldName() << ": undefined"; break;
	case jstNULL:
		s << fieldName() << ": null"; break;
	case MaxKey:
		s << fieldName() << ": MaxKey"; break;
	case Code:
		s << fieldName() << ": ";
		if( valuestrsize() > 80 ) 
			s << string(valuestr()).substr(0, 70) << "...";
		else { 
			s << valuestr();
		}
		break;
	case String:
		s << fieldName() << ": ";
		if( valuestrsize() > 80 ) 
			s << '"' << string(valuestr()).substr(0, 70) << "...\"";
		else { 
			s << '"' << valuestr() << '"';
		}
		break;
	case jstOID: 
          s << fieldName() << " : ObjId(";
          s << hex << oid().a << hex << oid().b << ')';
          break;
    default:
		s << fieldName() << ": ?type=" << type();
		break;
	}
	return s.str();
}

int Element::size() {
	if( totalSize >= 0 )
		return totalSize;

	int x = 1;
	switch( type() ) {
		case EOO:
		case Undefined:
		case jstNULL:
		case MaxKey:
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
		case Code:
		case String:
			x = valuestrsize() + 4 + 1;
			break;
        case DBRef:
          x = valuestrsize() + 4 + 12 + 1;
          break;
		case Object:
		case Array:
			x = objsize() + 1;
			break;
		case BinData:
			x = valuestrsize() + 4 + 1 + 1/*subtype*/;
			break;
		case RegEx:
			{
				const char *p = value();
				int len1 = strlen(p);
				p = p + len1 + 1;
				x = 1 + len1 + strlen(p) + 2;
			}
			break;
		default:
			cout << "Element: bad type " << (int) type() << endl;
			assert(false);
	}
	totalSize =  x + fieldNameSize;

	if( !eoo() ) { 
		const char *next = data + totalSize;
		if( *next < 0 || *next > JSTypeMax ) { 
			// bad type.  
			cout << "*********************************************\n";
			cout << "Bad data or size in Element::size()" << endl;
			cout << "bad type:" << (int) *next << endl;
			cout << "totalsize:" << totalSize << " fieldnamesize:" << fieldNameSize << endl;
			cout << "lastrec:" << endl;
			dumpmemory(data, totalSize + 15);
			assert(false);
		}
	}

	return totalSize;
}

/* must be same type! */
inline int compareElementValues(Element& l, Element& r) {
	int f;
	double x;
	switch( l.type() ) {
		case EOO:
		case Undefined:
		case jstNULL:
		case MaxKey:
			f = l.type() - r.type();
			if( f<0 ) return -1;
			return f==0 ? 0 : 1;
		case Bool:
			return *l.value() - *r.value();
		case Date:
			if( l.date() < r.date() )
				return -1;
			return l.date() == r.date() ? 0 : 1;
		case Number:
			x = l.number() - r.number();
			if( x < 0 ) return -1;
			return x == 0 ? 0 : 1;
		case jstOID:
			return memcmp(l.value(), r.value(), 12);
		case Code:
		case String:
			/* todo: utf version */
			return strcmp(l.valuestr(), r.valuestr());
		case Object:
		case Array:
		case DBRef:
			{
				int lsz = l.valuesize();
				int rsz = r.valuesize();
				if( lsz - rsz != 0 ) return lsz - rsz;
				return memcmp(l.value(), r.value(), lsz);
			}
		case BinData:
		case RegEx:
			cout << "compareElementValues: can't compare this type:" << (int) l.type() << endl;
			assert(false);
			break;
		default:
			cout << "compareElementValues: bad type " << (int) l.type() << endl;
			assert(false);
	}
	return -1;
}

/* JSMatcher --------------------------------------*/

// If the element is something like:
//   a : { $gt : 3 }
// we append
//   a : 3
// else we just append the element.
//
void appendElementHandlingGtLt(JSObjBuilder& b, Element& e) { 
	if( e.type() == Object ) {
		Element fe = e.embeddedObject().firstElement();
		const char *fn = fe.fieldName();
		if( fn[0] == '$' && fn[1] && fn[2] == 't' ) { 
			b.appendAs(fe, e.fieldName());
			return;
		}
	}
	b.append(e);
}

int getGtLtOp(Element& e) { 
	int op = JSMatcher::Equality;
	if( e.type() != Object ) 
		return op;

	Element fe = e.embeddedObject().firstElement();
	const char *fn = fe.fieldName();
	if( fn[0] == '$' && fn[1] && fn[2] == 't' ) { 
		if( fn[1] == 'g' ) { 
			if( fn[3] == 0 ) op = JSMatcher::GT;
			else if( fn[3] == 'e' && fn[4] == 0 ) op = JSMatcher::GTE;
		}
		else if( fn[1] == 'l' ) { 
			if( fn[3] == 0 ) op = JSMatcher::LT;
			else if( fn[3] == 'e' && fn[4] == 0 ) op = JSMatcher::LTE;
		}
	}
	return op;
}

JSMatcher::JSMatcher(JSObj &_jsobj) : 
   where(0), jsobj(_jsobj), nRegex(0)
{
	nBuilders = 0;

	JSElemIter i(jsobj);
	n = 0;
	while( i.more() ) {
		Element e = i.next();
		if( e.eoo() )
			break;

		if( e.type() == Code && strcmp(e.fieldName(), "$where")==0 ) { 
			// $where: function()...
			assert( where == 0 );
			where = new Where();
			const char *code = e.valuestr();
                        if ( ! JavaJS ){
                          JavaJS = new JavaJSImpl();
                          javajstest();
                        }
			where->scope = JavaJS->scopeCreate();
			where->func = JavaJS->functionCreate( code );
			continue;
		}

		if( e.type() == RegEx ) {
			if( nRegex >= 4 ) {
				cout << "ERROR: too many regexes in query" << endl;
			}
			else {
				pcrecpp::RE_Options options;
				options.set_utf8(true);
				const char *flags = e.regexFlags();
				while( flags && *flags ) { 
					if( *flags == 'i' )
						options.set_caseless(true);
					else if( *flags == 'm' )
						options.set_multiline(true);
					else if( *flags == 'x' )
						options.set_extended(true);
					flags++;
				}
				regexs[nRegex].re = new pcrecpp::RE(e.regex(), options);
				regexs[nRegex].fieldName = e.fieldName();
				nRegex++;
			}
			continue;
		}

		// greater than / less than...
		// { a : { $gt: 3 } }
		if( e.type() == Object ) {
			Element fe = e.embeddedObject().firstElement();
			const char *fn = fe.fieldName();
			if( fn[0] == '$' && fn[1] && fn[2] == 't' ) { 
				int op = Equality;
				if( fn[1] == 'g' ) { 
					if( fn[3] == 0 ) op = GT;
					else if( fn[3] == 'e' && fn[4] == 0 ) op = GTE;
				}
				else if( fn[1] == 'l' ) { 
					if( fn[3] == 0 ) op = LT;
					else if( fn[3] == 'e' && fn[4] == 0 ) op = LTE;
				}
				if( op && nBuilders < 8) { 
					JSObjBuilder *b = new JSObjBuilder();
					builders[nBuilders++] = b;
					b->appendAs(fe, e.fieldName());
					toMatch.push_back( b->done().firstElement() );
					compareOp.push_back(op);
					n++;
					continue;
				}
			}
		}

		{
			toMatch.push_back(e);
			compareOp.push_back(Equality);
			n++;
		}
	}
}

inline int JSMatcher::valuesMatch(Element& l, Element& r, int op) { 
	if( op == 0 ) 
		return l.valuesEqual(r);

	if( l.type() != r.type() )
		return false;

	int c = compareElementValues(l, r);
	int z = 1 << (c+1); 
	return (op & z);
}

/* deep means we looked into arrays for a match */
bool JSMatcher::matches(JSObj& jsobj, bool *deep) {
	if( deep ) 
		*deep = false;

	/* assuming there is usually only one thing to match.  if more this
	could be slow sometimes. */

	for( int r = 0; r < nRegex; r++ ) { 
		RegexMatcher& rm = regexs[r];
		JSElemIter k(jsobj);
		while( 1 ) {
			if( !k.more() )
				return false;
			Element e = k.next();
			if( strcmp(e.fieldName(), rm.fieldName) == 0 ) {
				char buf[64];
				const char *p = buf;
				if( e.type() == String )
					p = e.valuestr();
				else if( e.type() == Number ) { 
					sprintf(buf, "%f", e.number());
				}
				else if( e.type() == Date ) { 
					unsigned long long d = e.date();
					time_t t = (d/1000);
					time_t_to_String(t, buf);
				}
				else
					return false;
				if( !rm.re->PartialMatch(p) )
					return false;
				break;
			}
		}
	}

	// check normal non-regex cases:
	for( int i = 0; i < n; i++ ) {
		Element& m = toMatch[i]; 
		JSElemIter k(jsobj);
		while( k.more() ) {
			Element e = k.next();
			if( strcmp(e.fieldName(), m.fieldName())== 0 ) {
				if( valuesMatch(e, m, compareOp[i]) ) {
					goto ok;
				}
				else if( e.type() == Array ) {
					JSElemIter ai(e.embeddedObject());
					while( ai.more() ) { 
						Element z = ai.next();
						if( valuesMatch( z, m, compareOp[i]) ) {
							if( deep )
								*deep = true;
							goto ok;
						}
					}
				}
				return false;
			}
		}

		/* missing.  that is ok iff we were looking for null */
		if( m.type() == jstNULL || m.type() == Undefined )
			;
		else
			return false;
ok:
		;
	}

	if( where ) { 
		if( where->func == 0 )
			return false; // didn't compile
		JavaJS->scopeSetObject(where->scope, "obj", &jsobj);
		if( JavaJS->invoke(where->scope, where->func) )
			return false;
		double n = JavaJS->scopeGetNumber(where->scope, "return");
		return n != 0;
	}

	return true;
}

/* JSObj ------------------------------------------------------------*/

string JSObj::toString() const { 
	stringstream s;
	s << "{ ";
	JSElemIter i(*this);
	Element e = i.next();
	if( !e.eoo() )
	while( 1 ) { 
		s << e.toString();
		e = i.next();
		if( e.eoo() )
			break;
		s << ", ";
	}
	s << " }";
	return s.str();
}

/* well ordered compare */
int JSObj::woCompare(const JSObj& r) const { 

	if( isEmpty() )
		return r.isEmpty() ? 0 : -1;
	if( r.isEmpty() )
		return 1;

	JSElemIter i(*this);
	JSElemIter j(r);
	while( 1 ) { 
		// so far, equal...

		Element l = i.next();
		Element r = j.next();

		if( l == r ) {
			if( l.eoo() )
				return 0;
			continue;
		}

		int x = (int) l.type() - (int) r.type();
		if( x != 0 )
			return x;
		x = strcmp(l.fieldName(), r.fieldName());
		if( x != 0 )
			return x;
		x = compareElementValues(l, r);
		assert(x != 0);
		return x;
	}
	return -1;
} 

Element JSObj::getField(const char *name) {
	JSElemIter i(*this);
	while( i.more() ) {
		Element e = i.next();
		if( e.eoo() )
			break;
		if( strcmp(e.fieldName(), name) == 0 )
			return e;
	}
	return nullElement;
}

/* makes a new JSObj with the fields specified in pattern.
   fields returned in the order they appear in pattern.
   if any field missing, you get back an empty object overall.

   n^2 implementation bad if pattern and object have lots 
   of fields - normally pattern doesn't so should be fine.
*/
JSObj JSObj::extractFields(JSObj pattern, JSObjBuilder& b) { 
	JSElemIter i(pattern);
	while( i.more() ) {
		Element e = i.next();
		if( e.eoo() )
			break;
		Element x = getField(e.fieldName());
		if( x.eoo() )
			return JSObj();
		b.append(x);
	}
	return b.done();
}

const char * JSObj::getStringField(const char *name) { 
	Element e = getField(name);
	return e.type() == String ? e.valuestr() : 0;
}

JSObj JSObj::getObjectField(const char *name) { 
	Element e = getField(name);
	return e.type() == Object ? e.embeddedObject() : JSObj();
}

int JSObj::getFieldNames(set<string>& fields) {
	int n = 0;
	JSElemIter i(*this);
	while( i.more() ) {
		Element e = i.next();
		if( e.eoo() )
			break;
		fields.insert(e.fieldName());
		n++;
	}
	return n;
}

/* note: addFields always adds _id even if not specified 
   returns n added not counting _id unless requested.
*/
int JSObj::addFields(JSObj& from, set<string>& fields) {
	assert( _objdata == 0 ); /* partial implementation for now... */

	JSObjBuilder b;

	int N = fields.size();
	int n = 0;
	JSElemIter i(from);
	bool gotId = false;
	while( i.more() ) {
		Element e = i.next();
		const char *fname = e.fieldName();
		if( fields.count(fname) ) {
			b.append(e);
			++n;
			gotId = gotId || strcmp(fname, "_id")==0;
			if( n == N && gotId )
				break;
		} else if( strcmp(fname, "_id")==0 ) {
			b.append(e);
			gotId = true;
			if( n == N && gotId )
				break;
		}
	}

	if( n ) {
		_objdata = b.decouple(_objsize);
		iFree = true;
	}

	return n;
}

/*-- test things ----------------------------------------------------*/

#pragma pack(push)
#pragma pack(1)

struct MaxKeyData { 
	MaxKeyData() { totsize=7; maxkey=MaxKey; name=0; eoo=EOO; }
	int totsize;
	char maxkey;
	char name;
	char eoo;
} maxkeydata;
JSObj maxKey((const char *) &maxkeydata);

struct JSObj0 {
	JSObj0() { totsize = 5; eoo = EOO; }
	int totsize;
	char eoo;
} js0;

Element::Element() { 
	data = &js0.eoo;
	fieldNameSize = 0;
	totalSize = -1;
}

struct JSObj1 js1;

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

struct RXTest { 
	RXTest() { 
		/*
		static const boost::regex e("(\\d{4}[- ]){3}\\d{4}");
		static const boost::regex b(".....");
		cout << "regex result: " << regex_match("hello", e) << endl;
		cout << "regex result: " << regex_match("abcoo", b) << endl;
		*/
		pcrecpp::RE re1(")({a}h.*o");
		pcrecpp::RE re("h.llo");
		assert( re.FullMatch("hello") );
		assert( !re1.FullMatch("hello") );


		pcrecpp::RE_Options options;
		options.set_utf8(true);
		pcrecpp::RE part("dwi", options);
		assert( part.PartialMatch("dwight") );
	}
} rxtest;

