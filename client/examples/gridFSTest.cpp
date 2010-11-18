// gridFSTest.cpp

/*    Copyright 2010 10gen Inc.
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
 * some simple tests of the gridfs c++ api
 */

#include "client/dbclient.h"
#include "client/gridfs.h"

#include <iostream>

#include <stdlib.h>
#include <unistd.h>

#ifndef assert
#  define assert(x) MONGO_assert(x)
#endif

#define TMP_FILE "/tmp/gridFSTestTemp.XXXXXX"

using namespace std;
using namespace mongo;

int create_tmp_file(char *fname);
void test_simple(GridFS &gfs);
void test_removeFiles(GridFS &gfs);

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
    test_removeFiles(gfs);

}

int create_tmp_file(char *fname) {
  int fd = mkstemp(fname);
  if (fd == -1) {
    cerr << "unable to open tmp file: " << fname << endl;
    exit(1);
  }
    
  return fd;
}

/*
 * test some basic api usage
 *
 */
void test_simple(GridFS &gfs) {

  // create a temporary file on the filesystem to store in gridfs
  char filename[] = TMP_FILE;
  int fd = create_tmp_file(filename);

  // write a small string to the file
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
  assert(f3.getContentLength() == 0);
  
  close(fd);
  unlink(filename);
}

/*
 * test removeFiles
 */
void test_removeFiles(GridFS &gfs) {

  char filename1[] = TMP_FILE;
  char filename2[] = TMP_FILE;

  int fd1 = create_tmp_file(filename1);
  int fd2 = create_tmp_file(filename2);

  BSONObj f1 = gfs.storeFile(filename1);
  BSONObj f2 = gfs.storeFile(filename2);
  
  int deleted = gfs.removeFiles(BSON( "md5" << f1.getStringField("md5")));
  assert(deleted == 2);

  close(fd1);  close(fd2);
  unlink(filename1);  unlink(filename2);

}
