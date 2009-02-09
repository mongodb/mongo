#include <iostream>
#include "../../client/dbclient.h"

// g++ -I ../.. -L ../.. tutorial.cpp -lmongoclient -lboost_thread -lboost_filesystem

using namespace mongo;

void run() {
  DBClientConnection c;
  c.connect("localhost");
  cout << "connected ok" << endl;
  BSONObj p = BSON( "name" << "Joe" << "age" << 33 );
  c.insert("tutorial.persons", p);
}

int main() { 
  try { 
    run();
  } 
  catch( DBException &e ) { 
    cout << "caught " << e.what() << endl;
  }
  return 0;
}
