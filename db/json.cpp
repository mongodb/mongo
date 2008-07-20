// json.cpp

#include "stdafx.h"
#include "json.h"
#include "../util/builder.h"

/* partial implementation for now */

void skipWhite(const char *&p) { 
	while( *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t' )
		p++;
}

void value(JSObjBuilder& b, const char *&p, string& id) { 
	if( strncmp(p, "ObjId()", 7) == 0 ) {
		p += 7;
		b.appendOID(id.c_str());
	}
}

void _fromjson(JSObjBuilder& b, const char *&p) { 
	while( 1 ) { 
		skipWhite(p);
		if( *p == 0 )
			break;
		if( *p == '{' ) { _fromjson(b,++p); continue; }
		if( *p == '}' ) { ++p; break; }
		if( *p == '_' || isalpha(*p) ) { 
			string id;
			while( *p == '_' || isalpha(*p) || isdigit(*p)  ) { 
				id += *p++;
			}
			skipWhite(p);
			assert( *p == ':' ); p++;
			skipWhite(p);
			value(b, p, id);
			continue;
		}
	}
}

JSObj fromjson(const char *str) { 
	JSObjBuilder b;
	_fromjson(b,str);
	return b.doneAndDecouple();
}
