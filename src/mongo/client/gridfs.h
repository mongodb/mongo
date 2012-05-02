/** @file gridfs.h */

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

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclientinterface.h"

namespace mongo {

    typedef unsigned long long gridfs_offset;

    class GridFS;
    class GridFile;

    class GridFSChunk {
    public:
        GridFSChunk( BSONObj data );
        GridFSChunk( BSONObj fileId , int chunkNumber , const char * data , int len );

        int len() const {
            int len;
            _data["data"].binDataClean( len );
            return len;
        }

        const char * data( int & len ) const {
            return _data["data"].binDataClean( len );
        }

    private:
        BSONObj _data;
        friend class GridFS;
    };


    /**
      GridFS is for storing large file-style objects in MongoDB.
      @see http://www.mongodb.org/display/DOCS/GridFS+Specification
     */
    class GridFS {
    public:
        /**
         * @param client - db connection
         * @param dbName - root database name
         * @param prefix - if you want your data somewhere besides <dbname>.fs
         */
        GridFS( DBClientBase& client , const string& dbName , const string& prefix="fs" );
        ~GridFS();

        /**
         * @param
         */
        void setChunkSize(unsigned int size);

        /**
         * puts the file reference by fileName into the db
         * @param fileName local filename relative to process
         * @param remoteName optional filename to use for file stored in GridFS
         *                   (default is to use fileName parameter)
         * @param contentType optional MIME type for this object.
         *                    (default is to omit)
         * @return the file object
         */
        BSONObj storeFile( const string& fileName , const string& remoteName="" , const string& contentType="");

        /**
         * puts the file represented by data into the db
         * @param data pointer to buffer to store in GridFS
         * @param length length of buffer
         * @param remoteName optional filename to use for file stored in GridFS
         *                   (default is to use fileName parameter)
         * @param contentType optional MIME type for this object.
         *                    (default is to omit)
         * @return the file object
         */
        BSONObj storeFile( const char* data , size_t length , const string& remoteName , const string& contentType="");

        /**
         * removes file referenced by fileName from the db
         * @param fileName filename (in GridFS) of the file to remove
         * @return the file object
         */
        void removeFile( const string& fileName );

        /**
         * returns a file object matching the query
         */
        GridFile findFile( BSONObj query ) const;

        /**
         * equiv to findFile( { filename : filename } )
         */
        GridFile findFile( const string& fileName ) const;

        /**
         * convenience method to get all the files
         */
        auto_ptr<DBClientCursor> list() const;

        /**
         * convenience method to get all the files with a filter
         */
        auto_ptr<DBClientCursor> list( BSONObj query ) const;

    private:
        DBClientBase& _client;
        string _dbName;
        string _prefix;
        string _filesNS;
        string _chunksNS;
        unsigned int _chunkSize;

        // insert fileobject. All chunks must be in DB.
        BSONObj insertFile(const string& name, const OID& id, gridfs_offset length, const string& contentType);

        friend class GridFile;
    };

    /**
       wrapper for a file stored in the Mongo database
     */
    class GridFile {
    public:
        /**
         * @return whether or not this file exists
         * findFile will always return a GriFile, so need to check this
         */
        bool exists() const {
            return ! _obj.isEmpty();
        }

        string getFilename() const {
            return _obj["filename"].str();
        }

        int getChunkSize() const {
            return (int)(_obj["chunkSize"].number());
        }

        gridfs_offset getContentLength() const {
            return (gridfs_offset)(_obj["length"].number());
        }

        string getContentType() const {
            return _obj["contentType"].valuestr();
        }

        Date_t getUploadDate() const {
            return _obj["uploadDate"].date();
        }

        string getMD5() const {
            return _obj["md5"].str();
        }

        BSONElement getFileField( const string& name ) const {
            return _obj[name];
        }

        BSONObj getMetadata() const;

        int getNumChunks() const {
            return (int) ceil( (double)getContentLength() / (double)getChunkSize() );
        }

        GridFSChunk getChunk( int n ) const;

        /**
           write the file to the output stream
         */
        gridfs_offset write( ostream & out ) const;

        /**
           write the file to this filename
         */
        gridfs_offset write( const string& where ) const;

    private:
        GridFile(const GridFS * grid , BSONObj obj );

        void _exists() const;

        const GridFS * _grid;
        BSONObj        _obj;

        friend class GridFS;
    };
}
