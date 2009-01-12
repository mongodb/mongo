// first.cpp

/**
 * this is a good first example of how to use mongo from c++
 */

#include <iostream>

#include "../dbclient.h"

using namespace std;

void insert( DBClientConnection & conn , const char * name , int num ){
    BSONObjBuilder obj;
    obj.append( "name" , name );
    obj.append( "num" , num );
    conn.insert( "test.people" , obj.doneAndDecouple() );
}

int main(){

    DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( "127.0.0.1" , errmsg ) ){
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }

    { // clean up old data from any previous tests
        BSONObjBuilder query;
        conn.remove( "test.people" , query.doneAndDecouple() );
    }
                 
    insert( conn , "eliot" , 15 );
    insert( conn , "sara" , 23 );
    
    {
        BSONObjBuilder query;
        auto_ptr<DBClientCursor> cursor = conn.query( "test.people" , query.doneAndDecouple() );
        cout << "using cursor" << endl;
        while ( cursor->more() ){    
            BSONObj obj = cursor->next();
            cout << "\t" << obj.jsonString() << endl;
        }
        
    }
    
    {
        BSONObjBuilder query;
        query.append( "name" , "eliot" );
        BSONObj res = conn.findOne( "test.people" , query.doneAndDecouple() );
        cout << res.isEmpty() << "\t" << res.jsonString() << endl;
    }

    {
        BSONObjBuilder query;
        query.append( "name" , "asd" );
        BSONObj res = conn.findOne( "test.people" , query.doneAndDecouple() );
        cout << res.isEmpty() << "\t" << res.jsonString() << endl;
    }
    

}
