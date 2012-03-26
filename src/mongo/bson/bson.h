/** @file bson.h

    Main bson include file for mongodb c++ clients. MongoDB includes ../db/jsobj.h instead. 
    This file, however, pulls in much less code / dependencies.

    @see bsondemo
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
   Main include file for C++ BSON module when using standalone (sans MongoDB client).

   "BSON" stands for "binary JSON" -- ie a binary way to represent objects that would be
   represented in JSON (plus a few extensions useful for databases & other languages).

   http://www.bsonspec.org/
*/

#pragma once

#if defined(MONGO_EXPOSE_MACROS)
#error this header is for client programs, not the mongo database itself. include jsobj.h instead.
/* because we define simplistic assert helpers here that don't pull in a bunch of util -- so that
   BSON can be used header only.
   */
#endif

#include <cstdlib>
#include <memory>
#include <iostream>
#include <sstream>
//#include <boost/utility.hpp>

namespace bson {

    using std::string;
    using std::stringstream;

    class assertion : public std::exception {
    public:
        assertion( unsigned u , const std::string& s )
            : id( u ) , msg( s ) {
            std::stringstream ss;
            ss << "BsonAssertion id: " << u << " " << s;
            full = ss.str();
        }

        virtual ~assertion() throw() {}

        virtual const char* what() const throw() { return full.c_str(); }

        unsigned id;
        std::string msg;
        std::string full;
    };
}

namespace mongo {
#if !defined(verify)
    inline void verify(bool expr) {
        if(!expr) {
            throw bson::assertion( 0 , "assertion failure in bson library" );
        }
    }
#endif
#if !defined(uassert)
    inline void uasserted(unsigned msgid, std::string s) {
        throw bson::assertion( msgid , s );
    }

    inline void uassert(unsigned msgid, std::string msg, bool expr) {
        if( !expr )
            uasserted( msgid , msg );
    }
    inline void msgasserted(int msgid, const char *msg) {
        throw bson::assertion( msgid , msg );
    }
    inline void msgasserted(int msgid, const std::string &msg) { msgasserted(msgid, msg.c_str()); }
    inline void massert(unsigned msgid, std::string msg, bool expr) {
        if(!expr) {
            std::cout << "assertion failure in bson library: " << msgid << ' ' << msg << std::endl;
            throw bson::assertion( msgid , msg );
        }
    }
#endif
}

#include "util/builder.h"
#include "bsontypes.h"
#include "oid.h"
#include "bsonelement.h"
#include "bsonobj.h"
#include "bsonobjbuilder.h"
#include "bsonobjiterator.h"
#include "bson-inl.h"
