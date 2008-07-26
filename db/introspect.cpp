// introspect.cpp 

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
#include "introspect.h"
#include "../util/builder.h"
#include "../util/goodies.h"
#include "pdfile.h"
#include "jsobj.h"
#include "pdfile.h"

typedef map<string,Cursor*> StringToCursor;
StringToCursor *specialNamespaces;

auto_ptr<Cursor> getSpecialCursor(const char *ns) {
	StringToCursor::iterator it = specialNamespaces->find(ns);
	return auto_ptr<Cursor>
		(it == specialNamespaces->end() ? 
		0 : it->second->clone());
}

void SingleResultObjCursor::reg(const char *as) {
	if( specialNamespaces == 0 )
		specialNamespaces = new StringToCursor();
	if( specialNamespaces->count(as) == 0 ) {
		(*specialNamespaces)[as] = this;
	}
}

void profile(const char *str,
			 int millis)
{
	JSObjBuilder b;
	b.appendDate("ts", jsTime());
	b.append("info", str);
	b.append("millis", (double) millis);
	JSObj p = b.done();
	theDataFileMgr.insert(client->profileName.c_str(), 
		p.objdata(), p.objsize(), true);
}
