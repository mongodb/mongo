// capped2tests.cpp capped collections

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

#include "pch.h"

#include "pch.h"
#include "../db/namespace.h"
#include "../db/db.h"
#include "../db/json.h"
#include "dbtests.h"
#include "../db/capped.h"

namespace Capped2Tests {

    class Base { 
    public:
    };

    class C1 : public Base {
    public:
        void run() {
            dblock lk;
            NamespaceDetails nsd(DiskLoc(), true);
            cappedcollection::create(dbpath, "a.b", &nsd, 1);
            cappedcollection::close(&nsd);
            cappedcollection::open(dbpath, "a", &nsd);
        }
    };        

    class All : public Suite {
    public:
        All() : Suite( "capped2" ){
        }

        void setupTests(){
            add< C1 >();
        }
    } myall;

}

