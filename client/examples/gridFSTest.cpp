#include "client/dbclient.h"
#include "client/gridfs.h"

#include <iostream>

#include <stdlib.h>
#include <unistd.h>

#ifndef assert
#  define assert(x) MONGO_assert(x)
#endif

using namespace std;
using namespace mongo;

// basic gridfs api tests
void test_simple(GridFS &gfs);


int main( int argc, const char **argv ) {

    const char *port = "27017";
    if ( argc != 1 ) {
        if ( argc != 3 )
            throw -12;
        port = argv[ 2 ];
    }

    DBClientConnection conn;
    string errmsg;
    if ( ! conn.connect( string( "127.0.0.1:" ) + port , errmsg ) ) {
        cout << "couldn't connect : " << errmsg << endl;
        throw -11;
    }

    GridFS gfs(conn, "gfstest");

    test_simple(gfs);

}

void test_simple(GridFS &gfs) {
  
  char filename[] = "/tmp/gridFSTestTemp.XXXXXX";
  int fd = mkstemp(filename);  
  if (fd == -1) {
    cerr << "unable to open tmp file: " << filename << endl;
    exit(1);
  }
  ssize_t written = write(fd, "foo", 4);
  
  BSONObj foo = gfs.storeFile(filename);
  assert(written == foo.getIntField("length"));
  
  GridFile f1 = gfs.findFile(filename);
  assert(f1.exists());
  assert(f1.getMD5() == foo.getStringField("md5"));
  assert((int)f1.getContentLength() == foo.getIntField("length"));
  assert(f1.getFilename() == foo.getStringField("filename"));
  assert(f1.getUploadDate() == foo["uploadDate"].date());
  
  GridFile f2 = gfs.findFile(BSON( "md5" << foo.getStringField("md5")));
  assert(f2.exists());
  assert(f2.getMD5() == foo.getStringField("md5"));
  assert((int)f2.getContentLength() == foo.getIntField("length"));
  assert(f2.getFilename() == foo.getStringField("filename"));
  assert(f2.getUploadDate() == foo["uploadDate"].date());
  
  gfs.removeFile(filename);
  GridFile f3 = gfs.findFile(BSON( "md5" << foo.getStringField("md5")));
  assert(!f3.exists());
  assert(f3.getContentLength() == 0);
  
  close(fd);
  unlink(filename);
}
