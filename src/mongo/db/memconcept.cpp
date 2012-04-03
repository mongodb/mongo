#include "pch.h"

#if 1

#define DDD(x) 

#include <string>
#include <map>
#include "memconcept.h"
#include "../util/assert_util.h"
#include <boost/functional/hash.hpp>
using namespace std;
#include "../util/log.h"
#include "../util/startup_test.h"

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
        void is(void *p, concept c, string description, unsigned len) {
            DDD( log() << "is  " << p << ' ' << c.toString() << ' ' << description << ' ' << len << endl; )
            C &node = map.find(p);
            node.p = p;
            node.c = c;
            node.len = len;
            strncpy(node.desc, description.c_str(), 15);
        }

        void invalidate(void *p, unsigned len) {
            DDD( log() << "inv " << p << " invalidate" << endl; )
            C &node = map.find(p);
            node.p = p;
            node.c = concept::err;
            // len is not used currenntly. hmmm.
        }
#endif

    }
}

#endif
