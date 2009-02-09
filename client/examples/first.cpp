// first.cpp

/**
 * this is a good first example of how to use mongo from c++
 */

#include <iostream>

#include "client/dbclient.h"

void insert( mongo::DBClientConnection & conn , const char * name , int num ) {
    mongo::BSONObjBuilder obj;
    obj.append( "name" , name );
    obj.append( "num" , num );
    conn.insert( "test.people" , obj.obj() );
}

int main() {

    mongo::DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( "127.0.0.1" , errmsg ) ) {
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }

    { // clean up old data from any previous tests
        mongo::BSONObjBuilder query;
        conn.remove( "test.people" , query.obj() );
    }

    insert( conn , "eliot" , 15 );
    insert( conn , "sara" , 23 );
    
    {
        mongo::BSONObjBuilder query;
        auto_ptr<mongo::DBClientCursor> cursor = conn.query( "test.people" , query.obj() );
        cout << "using cursor" << endl;
        while ( cursor->more() ) {
            mongo::BSONObj obj = cursor->next();
            cout << "\t" << obj.jsonString() << endl;
        }

    }
    
    {
        mongo::BSONObjBuilder query;
        query.append( "name" , "eliot" );
        mongo::BSONObj res = conn.findOne( "test.people" , query.obj() );
        cout << res.isEmpty() << "\t" << res.jsonString() << endl;
    }
    
    {
        mongo::BSONObjBuilder query;
        query.append( "name" , "asd" );
        mongo::BSONObj res = conn.findOne( "test.people" , query.obj() );
        cout << res.isEmpty() << "\t" << res.jsonString() << endl;
    }


}
