// json.cpp

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
