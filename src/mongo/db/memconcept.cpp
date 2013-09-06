
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

#if 1

#define DDD(x) 

#include "mongo/db/memconcept.h"

#include <boost/functional/hash.hpp>
#include <map>
#include <string>

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/startup_test.h"

using namespace std;

namespace mongo {
    namespace memconcept { 

        concept::concept(const char * desc) : c(desc) { }

        // these string pointers we use as unique identifiers - like enums. thus it is important
        // you don't use another with the same literal name 
        concept concept::err("err");
        concept concept::something("something");
        concept concept::database("database");
        concept concept::other("other");
        concept concept::memorymappedfile("memorymappedfile");
        concept concept::nsdetails("nsdetails");
        concept concept::datafileheader("datafileheader");
        concept concept::extent("extent");
        concept concept::record("record");
        concept concept::deletedrecord("deletedrecord");
        concept concept::btreebucket("btreebucket");

        class X : public StartupTest { 
        public:
            virtual void run() {
            }
        } concepttest;

        struct C { 
            void *p;
            unsigned len;
            concept c;
            char desc[16];
            string toString() const;
        };

        string C::toString() const { 
            stringstream ss;
            ss << p << ' ' << c.toString() << ' ' << len << ' ' << desc;
            return ss.str();
        }

        const int N = 100003;

        class map { 
            C nodes[N];
            boost::hash<void *> h;
        public:
            C& find(void *p) { 
                unsigned x = h(p);
                return nodes[x % N];
            }
            map() { 
                memset(this, 0, sizeof(*this));
                for( int i = 0; i < N; i++ )
                    nodes[i].c = concept::err;
            }
            void dump();
        } map;

        void map::dump() {
            // sort
            std::map<void*,C*> m;
            for( int i = 0; i < N; i++ ) { 
                if( nodes[i].p ) { 
                    m[ nodes[i].p ] = &nodes[i];
                }
            }
            // print
            for( std::map<void*,C*>::const_iterator i = m.begin(); i != m.end(); i++ ) { 
                log() << i->second->toString() << endl;
            }
        }

#if 0 && defined(_DEBUG)
        bool d = false;
        void is(void *p, concept c, const StringData& description, unsigned len) {
            DDD( log() << "is  " << p << ' ' << c.toString() << ' ' << description.data() << ' ' << len << endl; )
            C &node = map.find(p);
            node.p = p;
            node.c = c;
            node.len = len;
            strncpy(node.desc, description.data(), 15);
        }

        void invalidate(void *p, unsigned len) {
            DDD( log() << "inv " << p << " invalidate" << endl; )
            C &node = map.find(p);
            node.p = p;
            node.c = concept::err;
            // len is not used currently. hmmm.
        }
#endif

    }
}

#endif
