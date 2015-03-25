/** @file gridfs.h */

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
      @see http://dochub.mongodb.org/core/gridfsspec
     */
    class GridFS {
    public:
        /**
         * @param client - db connection
         * @param dbName - root database name
         * @param prefix - if you want your data somewhere besides <dbname>.fs
         */
        GridFS( DBClientBase& client , const std::string& dbName , const std::string& prefix="fs" );
        ~GridFS();

        /**
         * @param
         */
        void setChunkSize(unsigned int size);

        unsigned int getChunkSize() const;

        /**
         * puts the file reference by fileName into the db
         * @param fileName local filename relative to process
         * @param remoteName optional filename to use for file stored in GridFS
         *                   (default is to use fileName parameter)
         * @param contentType optional MIME type for this object.
         *                    (default is to omit)
         * @return the file object
         */
        BSONObj storeFile( const std::string& fileName , const std::string& remoteName="" , const std::string& contentType="");

        /**
         * puts the file represented by data into the db
         * @param data pointer to buffer to store in GridFS
         * @param length length of buffer
         * @param remoteName filename to use for file stored in GridFS
         * @param contentType optional MIME type for this object.
         *                    (default is to omit)
         * @return the file object
         */
        BSONObj storeFile( const char* data , size_t length , const std::string& remoteName , const std::string& contentType="");

        /**
         * removes file referenced by fileName from the db
         * @param fileName filename (in GridFS) of the file to remove
         * @return the file object
         */
        void removeFile( const std::string& fileName );

        /**
         * returns a file object matching the query
         */
        GridFile findFile( BSONObj query ) const;

        /**
         * equiv to findFile( { filename : filename } )
         */
        GridFile findFile( const std::string& fileName ) const;

        /**
         * convenience method to get all the files
         */
        std::auto_ptr<DBClientCursor> list() const;

        /**
         * convenience method to get all the files with a filter
         */
        std::auto_ptr<DBClientCursor> list( BSONObj query ) const;

    private:
        DBClientBase& _client;
        std::string _dbName;
        std::string _prefix;
        std::string _filesNS;
        std::string _chunksNS;
        unsigned int _chunkSize;

        // insert fileobject. All chunks must be in DB.
        BSONObj insertFile(const std::string& name, const OID& id, gridfs_offset length, const std::string& contentType);

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

        std::string getFilename() const {
            return _obj["filename"].str();
        }

        int getChunkSize() const {
            return (int)(_obj["chunkSize"].number());
        }

        gridfs_offset getContentLength() const {
            return (gridfs_offset)(_obj["length"].number());
        }

        std::string getContentType() const {
            return _obj["contentType"].valuestr();
        }

        Date_t getUploadDate() const {
            return _obj["uploadDate"].date();
        }

        std::string getMD5() const {
            return _obj["md5"].str();
        }

        BSONElement getFileField( const std::string& name ) const {
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
        gridfs_offset write( std::ostream & out ) const;

        /**
           write the file to this filename
         */
        gridfs_offset write( const std::string& where ) const;

    private:
        GridFile(const GridFS * grid , BSONObj obj );

        void _exists() const;

        const GridFS * _grid;
        BSONObj        _obj;

        friend class GridFS;
    };
}
