// introspect.cpp 

#include "stdafx.h"
#include "introspect.h"
#include "../util/builder.h"
#include "pdfile.h"
#include "jsobj.h"
#include "pdfile.h"

typedef map<string,Cursor*> StringToCursor;
StringToCursor *specialNamespaces;
Profile profile;

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
