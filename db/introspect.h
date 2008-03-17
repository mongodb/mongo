// introspect.h
// system management stuff.

#pragma once

#include "../stdafx.h"
#include "jsobj.h"
#include "pdfile.h"

auto_ptr<Cursor> getSpecialCursor(const char *ns);

class SingleResultObjCursor : public Cursor {
	int i;
protected:
	JSObjBuilder b;
	void reg(const char *as); /* register as a certain namespace */
public:
	SingleResultObjCursor() { i = 0; }
	virtual bool ok() { return i == 0; }
	virtual Record* _current() { assert(false); return 0; }
	virtual DiskLoc currLoc() { assert(false); return DiskLoc(); }

	virtual void fill() = 0;

	virtual JSObj current() {
		assert(i == 0);
		fill();
		return b.done();
	}

	virtual bool advance() {
		i++;
		return false;
	}

	virtual const char * toString() { return "SingleResultObjCursor"; }

};

/* --- profiling -------------------------------------------- 
   do when client->profile is set
*/

void profile(const char *str,
			 int millis);

