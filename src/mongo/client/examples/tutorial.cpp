//tutorial.cpp

/*    Copyright 2009 10gen Inc.
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

#include <iostream>
#include "mongo/client/dbclient.h"

// g++ src/mongo/client/examples/tutorial.cpp -pthread -Isrc -Isrc/mongo -lmongoclient -lboost_thread-mt -lboost_system -lboost_filesystem -L[path to libmongoclient.a] -o tutorial
//g++ tutorial.cpp -L[mongo directory] -L/opt/local/lib -lmongoclient -lboost_thread-mt -lboost_filesystem -lboost_system -I/opt/local/include  -o tutorial

using namespace mongo;

int printIfAge(DBClientConnection& c, int age) {
    std::auto_ptr<DBClientCursor> cursor = c.query("tutorial.persons", QUERY( "age" << age ).sort("name") );
    if (!cursor.get()) {
        cout << "query failure" << endl;
        return EXIT_FAILURE;
    }

    while( cursor->more() ) {
        BSONObj p = cursor->next();
        cout << p.getStringField("name") << endl;
    }
    return EXIT_SUCCESS;
}

int run() {
    DBClientConnection c;
    c.connect("localhost"); //"192.168.58.1");
    cout << "connected ok" << endl;
    BSONObj p = BSON( "name" << "Joe" << "age" << 33 );
    c.insert("tutorial.persons", p);
    p = BSON( "name" << "Jane" << "age" << 40 );
    c.insert("tutorial.persons", p);
    p = BSON( "name" << "Abe" << "age" << 33 );
    c.insert("tutorial.persons", p);
    p = BSON( "name" << "Methuselah" << "age" << BSONNULL);
    c.insert("tutorial.persons", p);
    p = BSON( "name" << "Samantha" << "age" << 21 << "city" << "Los Angeles" << "state" << "CA" );
    c.insert("tutorial.persons", p);

    c.ensureIndex("tutorial.persons", fromjson("{age:1}"));

    cout << "count:" << c.count("tutorial.persons") << endl;

    std::auto_ptr<DBClientCursor> cursor = c.query("tutorial.persons", BSONObj());
    if (!cursor.get()) {
        cout << "query failure" << endl;
        return EXIT_FAILURE;
    }

    while( cursor->more() ) {
        cout << cursor->next().toString() << endl;
    }

    cout << "\nprintifage:\n";
    return printIfAge(c, 33);
}

int main() {
    int ret = EXIT_SUCCESS;
    try {
        ret = run();
    }
    catch( DBException &e ) {
        cout << "caught " << e.what() << endl;
        ret = EXIT_FAILURE;
    }
    return ret;
}
