// introspect.h
// system management stuff.

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

#pragma once

#include "../stdafx.h"
#include "jsobj.h"
#include "pdfile.h"

namespace mongo {

auto_ptr<Cursor> getSpecialCursor(const char *ns);

class SingleResultObjCursor : public Cursor {
    int i;
protected:
    BSONObjBuilder b;
    void reg(const char *as); /* register as a certain namespace */
public:
    SingleResultObjCursor() {
        i = 0;
    }
    virtual bool ok() {
        return i == 0;
    }
    virtual Record* _current() {
        assert(false);
        return 0;
    }
    virtual DiskLoc currLoc() {
        assert(false);
        return DiskLoc();
    }

    virtual void fill() = 0;

    virtual BSONObj current() {
        assert(i == 0);
        fill();
        return b.done();
    }

    virtual bool advance() {
        i++;
        return false;
    }

    virtual string toString() {
        return "SingleResultObjCursor";
    }

};

/* --- profiling --------------------------------------------
   do when database->profile is set
*/

void profile(const char *str,
             int millis);

} // namespace mongo
