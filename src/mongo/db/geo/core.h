// core.h

/**
*    Copyright (C) 2008-2012 10gen Inc.
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

#include "mongo/pch.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

#include <cmath>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#if 0
# define CDEBUG -1
#else
# define CDEBUG 10
#endif

#if 0
# define GEODEBUGGING
# define GEODEBUG(x) cout << x << endl;
# define GEODEBUGPRINT(x) PRINT(x)
    inline void PREFIXDEBUG(GeoHash prefix, const GeoConvert* g) {
        if (!prefix.constrains()) {
            cout << "\t empty prefix" << endl;
            return ;
        }

        Point ll (g, prefix); // lower left
        prefix.move(1,1);
        Point tr (g, prefix); // top right

        Point center ((ll._x+tr._x)/2, (ll._y+tr._y)/2);
        double radius = fabs(ll._x - tr._x) / 2;

        cout << "\t ll: " << ll.toString() << " tr: " << tr.toString()
             << " center: " << center.toString() << " radius: " << radius << endl;

    }
#else
# define GEODEBUG(x)
# define GEODEBUGPRINT(x)
# define PREFIXDEBUG(x, y)
#endif

// Used by haystack.cpp.  XXX: change to something else/only have one of these geo things/nuke em
// all?
#define GEOQUADDEBUG(x)
//#define GEOQUADDEBUG(x) cout << x << endl

// XXX: move elsewhere?
namespace mongo {
    inline double deg2rad(const double deg) { return deg * (M_PI / 180.0); }
    inline double rad2deg(const double rad) { return rad * (180.0 / M_PI); }
}
