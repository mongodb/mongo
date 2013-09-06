/**
*    Copyright (C) 2012 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/


/* The idea here is to 'name' memory pointers so that we can do diagnostics.
   these diagnostics might involve concurrency or other things.  mainly would
   be for _DEBUG builds.  Experimental we'll see how useful.
*/

#pragma once

#include "mongo/base/string_data.h"

namespace mongo {
    namespace memconcept {

        /** these are like fancy enums - you can use them as "types" of things 
             and see if foo.concept == bar.concept.
            copyable.
        */
        class concept { 
        public:
            concept() { *this = err; }
            const char * toString() const { return c; }
            static concept err;
            static concept something;
            static concept database;
            static concept other;
            static concept memorymappedfile;
            static concept nsdetails;
            static concept datafileheader;
            static concept extent;
            static concept record;
            static concept deletedrecord;
            static concept btreebucket;
        private:
            const char * c;
            concept(const char *);
        };
        
        /** file was unmapped or something */
        void invalidate(void *p, unsigned len=0);

        /** note you can be more than one thing; a datafile header is also the starting pointer
            for a file */
        void is(void *p, concept c, const StringData& desc = StringData( "", 0 ), unsigned len=0);

#if 1
//#if !defined(_DEBUG)
        inline void invalidate(void *p, unsigned len) { }
        inline void is(void *p, concept c, const StringData& desc, unsigned) { }
#endif

    }
}
