// introspect.cpp 

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
