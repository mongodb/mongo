/* NOTE: This file is not quite ready to use yet.  Work in progress.
         include ../db/jsobj.h for now.

         This file, however, pulls in much less code.
*/

/** @file bson.h 
    BSON classes
*/

/*
 *    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
   BSONObj and its helpers

   "BSON" stands for "binary JSON" -- ie a binary way to represent objects that would be
   represented in JSON (plus a few extensions useful for databases & other languages).

   http://www.bsonspec.org/
*/

#pragma once

#include <iostream>
#include <sstream>

namespace mongo {
#if !defined(assert) 
    inline void assert(bool expr) {
        if(!expr) std::cout << "assertion failure in bson library" << std::endl;
    }
#endif
#if !defined(uassert)
    inline void uassert(unsigned msgid, const char *msg, bool expr) {
        if(!expr) std::cout << "assertion failure in bson library: " << msgid << ' ' << msg << std::endl;
    }
    inline void massert(unsigned msgid, std::string msg, bool expr) { 
        if(!expr) std::cout << "assertion failure in bson library: " << msgid << ' ' << msg << std::endl;
    }
#endif
}

#include "../bson/bsontypes.h"
#include "../bson/oid.h"
#include "../bson/bsonelement.h"
#include "../bson/bsonobj.h"
#include "../bson/bsonmisc.h"
#include "../bson/bsonobjbuilder.h"
#include "../bson/bsonobjiterator.h"
#include "../bson/bsoninlines.h"

namespace mongo { 

    inline unsigned getRandomNumber() {
#if defined(_WIN32)
        return rand();
#else
        return random(); 
#endif
    }

}
