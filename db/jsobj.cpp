// jsobj.cpp

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

#include "stdafx.h"
#include "jsobj.h"
#include "../util/goodies.h"
#include <limits>
#include "../util/unittest.h"

BSONElement nullElement;

string BSONElement::toString() const { 
	stringstream s;
	switch( type() ) {
    case EOO:
		return "EOO";
    case Date:
		s << fieldName() << ": Date(" << hex << date() << ')'; break;
    case RegEx:
        {
            s << fieldName() << ": /" << regex() << '/'; 
            const char *p = regexFlags();
            if( p ) s << p;
        }
        break;
	case NumberDouble:
	case NumberInt:
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
	case MinKey:
		s << fieldName() << ": MinKey"; break;
    case CodeWScope:
        s << fieldName() << ": codewscope"; break;
	case Code:
		s << fieldName() << ": ";
		if( valuestrsize() > 80 ) 
			s << string(valuestr()).substr(0, 70) << "...";
		else { 
			s << valuestr();
		}
		break;
    case Symbol:
	case String:
		s << fieldName() << ": ";
		if( valuestrsize() > 80 ) 
			s << '"' << string(valuestr()).substr(0, 70) << "...\"";
		else { 
			s << '"' << valuestr() << '"';
		}
		break;
	case DBRef: 
		s << fieldName();
		s << " : DBRef('" << valuestr() << "',";
		{
			OID *x = (OID *) (valuestr() + valuestrsize());
			s << hex << x->a << x->b << dec << ')';
		}
		break;
	case jstOID: 
		s << fieldName() << " : ObjId(";
		s << hex << oid().a << oid().b << dec << ')';
		break;
    default:
		s << fieldName() << ": ?type=" << type();
		break;
	}
	return s.str();
}

int BSONElement::size() const {
	if( totalSize >= 0 )
		return totalSize;

	int x = 1;
	switch( type() ) {
		case EOO:
		case Undefined:
		case jstNULL:
		case MaxKey:
		case MinKey:
			break;
		case Bool:
			x = 2;
			break;
        case NumberInt:
            x = 5;
            break;
		case Date:
		case NumberDouble:
			x = 9;
			break;
		case jstOID:
			x = 13;
			break;
                case Symbol:
		case Code:
		case String:
			x = valuestrsize() + 4 + 1;
			break;
	        case CodeWScope:
			x = objsize() + 1;
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
			cout << "BSONElement: bad type " << (int) type() << endl;
			assert(false);
	}
	((BSONElement *) this)->totalSize =  x + fieldNameSize;

	if( !eoo() ) { 
		const char *next = data + totalSize;
		if( *next < MinKey || ( *next > JSTypeMax && *next != MaxKey ) ) { 
			// bad type.  
			cout << "***\n";
			cout << "Bad data or size in BSONElement::size()\n";
			cout << "bad type:" << (int) *next << '\n';
			cout << "totalsize:" << totalSize << " fieldnamesize:" << fieldNameSize << '\n';
			cout << "lastrec:" << endl;
			//dumpmemory(data, totalSize + 15);
			assert(false);
		}
	}

	return totalSize;
}

int BSONElement::getGtLtOp() const {
	const char *fn = fieldName();
	if( fn[0] == '$' && fn[1] ) { 
		if( fn[2] == 't' ) { 
			if( fn[1] == 'g' ) { 
				if( fn[3] == 0 ) return JSMatcher::GT;
				else if( fn[3] == 'e' && fn[4] == 0 ) return JSMatcher::GTE;
			}
			else if( fn[1] == 'l' ) { 
				if( fn[3] == 0 ) return JSMatcher::LT;
				else if( fn[3] == 'e' && fn[4] == 0 ) return JSMatcher::LTE;
			}
		}
        else if( fn[2] == 'e' ) {
            if( fn[1] == 'n' && fn[3] == 0 )
                return JSMatcher::NE;
        }
		else if( fn[1] == 'i' && fn[2] == 'n' && fn[3] == 0 )
			return JSMatcher::opIN;
	}
	return JSMatcher::Equality;
}

int BSONElement::woCompare( const BSONElement &e,
						   bool considerFieldName ) const {
	int lt = (int) type();
	if( lt == NumberInt ) lt = NumberDouble;
	int rt = (int) e.type();
	if( rt == NumberInt ) rt = NumberDouble;
	
	int x = lt - rt;
	if( x != 0 )
		return x;
	if( considerFieldName ) {
		x = strcmp(fieldName(), e.fieldName());
		if( x != 0 )
			return x;
	}
	x = compareElementValues(*this, e);
	return x;	
}

/* must be same type! */
int compareElementValues(const BSONElement& l, const BSONElement& r) {
	int f;
	double x;
	switch( l.type() ) {
		case EOO:
		case Undefined:
		case jstNULL:
		case MaxKey:
		case MinKey:
			f = l.type() - r.type();
			if( f<0 ) return -1;
			return f==0 ? 0 : 1;
		case Bool:
			return *l.value() - *r.value();
		case Date:
			if( l.date() < r.date() )
				return -1;
			return l.date() == r.date() ? 0 : 1;
		case NumberInt:
		case NumberDouble:
			x = l.number() - r.number();
			if( x < 0 ) return -1;
			return x == 0 ? 0 : 1;
		case jstOID:
			return memcmp(l.value(), r.value(), 12);
		case Code:
                case Symbol:
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
void appendElementHandlingGtLt(BSONObjBuilder& b, BSONElement& e) { 
	if( e.type() == Object ) {
		BSONElement fe = e.embeddedObject().firstElement();
		const char *fn = fe.fieldName();
		if( fn[0] == '$' && fn[1] && fn[2] == 't' ) { 
			b.appendAs(fe, e.fieldName());
			return;
		}
	}
	b.append(e);
}

int getGtLtOp(BSONElement& e) { 
	if( e.type() != Object ) 
		return JSMatcher::Equality;

	BSONElement fe = e.embeddedObject().firstElement();
	return fe.getGtLtOp();
}


/* BSONObj ------------------------------------------------------------*/

string BSONObj::toString() const {
	if( isEmpty() ) return "{}";

	stringstream s;
	s << "{ ";
	BSONObjIterator i(*this);
	BSONElement e = i.next();
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

// todo: can be a little faster if we don't use toString() here.
bool BSONObj::valid() const { 
    try { 
        toString();
    }
    catch(...) { 
        return false;
    }
    return true;
}

/* well ordered compare */
int BSONObj::woCompare(const BSONObj& r) const { 
	if( isEmpty() )
		return r.isEmpty() ? 0 : -1;
	if( r.isEmpty() )
		return 1;

	BSONObjIterator i(*this);
	BSONObjIterator j(r);
	while( 1 ) { 
		// so far, equal...

		BSONElement l = i.next();
		BSONElement r = j.next();
		if ( l.eoo() )
			return 0;
		
		int x = l.woCompare( r );
		if ( x != 0 )
			return x;
	}
	return -1;
} 

BSONElement BSONObj::getField(const char *name) const {
    if( details ) {
        BSONObjIterator i(*this);
        while( i.more() ) {
            BSONElement e = i.next();
            if( e.eoo() )
                break;
            if( strcmp(e.fieldName(), name) == 0 )
                return e;
        }
    }
	return nullElement;
}

/* return has eoo() true if no match 
   supports "." notation to reach into embedded objects
*/
BSONElement BSONObj::getFieldDotted(const char *name) const {
	BSONElement e = getField( name );
	if( e.eoo() ) {
		const char *p = strchr(name, '.');
		if( p ) { 
			string left(name, p-name);
			BSONObj sub = getObjectField(left.c_str());
			return sub.isEmpty() ? nullElement : sub.getFieldDotted(p+1);
		}
	}

    return e;
/*
	BSONObjIterator i(*this);
	while( i.more() ) {
		BSONElement e = i.next();
		if( e.eoo() )
			break;
		if( strcmp(e.fieldName(), name) == 0 )
			return e;
	}
	return nullElement;
*/
}

/* makes a new BSONObj with the fields specified in pattern.
   fields returned in the order they appear in pattern.
   if any field missing, you get back an empty object overall.

   n^2 implementation bad if pattern and object have lots 
   of fields - normally pattern doesn't so should be fine.
*/
BSONObj BSONObj::extractFieldsDotted(BSONObj pattern, BSONObjBuilder& b) { 
	BSONObjIterator i(pattern);
	while( i.more() ) {
		BSONElement e = i.next();
		if( e.eoo() )
			break;
		BSONElement x = getFieldDotted(e.fieldName());
		if( x.eoo() )
			return BSONObj();
		b.append(x);
	}
	return b.done();
}
BSONObj BSONObj::extractFieldsUnDotted(BSONObj pattern) { 
    BSONObjBuilder b;
	BSONObjIterator i(pattern);
	while( i.more() ) {
		BSONElement e = i.next();
		if( e.eoo() )
			break;
		BSONElement x = getField(e.fieldName());
		if( x.eoo() )
			return BSONObj();
		b.append(x);
	}
	return b.doneAndDecouple();
}

BSONObj BSONObj::extractFields(BSONObj& pattern) { 
	BSONObjBuilder b(32); // scanandorder.h can make a zillion of these, so we start the allocation very small
	BSONObjIterator i(pattern);
	while( i.more() ) {
		BSONElement e = i.next();
		if( e.eoo() )
			break;
		BSONElement x = getFieldDotted(e.fieldName());
		if( x.eoo() )
			return BSONObj();
		b.append(x);
	}
	return b.doneAndDecouple();
}

int BSONObj::getIntField(const char *name) { 
	BSONElement e = getField(name);
	return e.isNumber() ? (int) e.number() : INT_MIN;
}

bool BSONObj::getBoolField(const char *name) { 
	BSONElement e = getField(name);
	return e.type() == Bool ? e.boolean() : false;
}

const char * BSONObj::getStringField(const char *name) { 
	BSONElement e = getField(name);
	return e.type() == String ? e.valuestr() : "";
}

BSONObj BSONObj::getObjectField(const char *name) const { 
	BSONElement e = getField(name);
	BSONType t = e.type();
	return t == Object || t == Array ? e.embeddedObject() : BSONObj();
}

int BSONObj::nFields() {
    int n = 0;
	BSONObjIterator i(*this);
	while( i.more() ) {
		BSONElement e = i.next();
		if( e.eoo() )
			break;
		n++;
	}
	return n;
}

/* grab names of all the fields in this object */
int BSONObj::getFieldNames(set<string>& fields) {
	int n = 0;
	BSONObjIterator i(*this);
	while( i.more() ) {
		BSONElement e = i.next();
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
int BSONObj::addFields(BSONObj& from, set<string>& fields) {
	assert( details == 0 ); /* partial implementation for now... */

	BSONObjBuilder b;

	int N = fields.size();
	int n = 0;
	BSONObjIterator i(from);
	bool gotId = false;
	while( i.more() ) {
		BSONElement e = i.next();
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
		int len;
		init( b.decouple(len), true );
	}

	return n;
}

/*-- test things ----------------------------------------------------*/

#pragma pack(push,1)
struct MaxKeyData { 
	MaxKeyData() { totsize=7; maxkey=MaxKey; name=0; eoo=EOO; }
	int totsize;
	char maxkey;
	char name;
	char eoo;
} maxkeydata;
BSONObj maxKey((const char *) &maxkeydata);

struct MinKeyData { 
	MinKeyData() { totsize=7; minkey=MinKey; name=0; eoo=EOO; }
	int totsize;
	char minkey;
	char name;
	char eoo;
} minkeydata;
BSONObj minKey((const char *) &minkeydata);

struct JSObj0 {
	JSObj0() { totsize = 5; eoo = EOO; }
	int totsize;
	char eoo;
} js0;
#pragma pack(pop)

BSONElement::BSONElement() { 
	data = &js0.eoo;
	fieldNameSize = 0;
	totalSize = -1;
}

#pragma pack(push,1)
struct EmptyObject {
	EmptyObject() { len = 5; jstype = EOO; }
	int len;
	char jstype;
} emptyObject;
#pragma pack(pop)

BSONObj emptyObj((char *) &emptyObject);

struct BsonUnitTest : public UnitTest { 
	void run() { 
        BSONObjBuilder A,B,C;
        A.appendInt("x", 2);
        B.append("x", 2.0);
        C.append("x", 2.1);
        BSONObj a = A.done();
        BSONObj b = B.done();
        BSONObj c = C.done();
        assert( !(a==b) ); // comments on operator==
        int cmp = a.woCompare(b);
        assert( cmp == 0 );
        cmp = a.woCompare(c);
        assert( cmp < 0 );
    }
} bsonut;
