/** @file gridfs.h */

#pragma once

#include "dbclient.h"

namespace mongo {

    typedef unsigned long long gridfs_offset;

    class GridFS;
    class GridFile;

    class Chunk {
    public:
        Chunk( BSONObj data );
        Chunk( BSONObj fileId , int chunkNumber , const char * data , int len );

        int len(){
            int len;
            const char * data = _data["data"].binData( len );
            int * foo = (int*)data;
            assert( len - 4 == foo[0] );
            return len - 4;
        }

        const char * data( int & len ){
            const char * data = _data["data"].binData( len );
            int * foo = (int*)data;
            assert( len - 4 == foo[0] );

            len = len - 4;
            return data + 4;
        }

    private:
        BSONObj _data;
        friend class GridFS;
    };


    /**
       this is the main entry point into the mongo grid fs
     */
    class GridFS{
    public:
        /**
         * @param client - db connection
         * @param dbName - root database name
         * @param prefix - if you want your data somewhere besides <dbname>.fs
         */
        GridFS( DBClientBase& client , const string& dbName , const string& prefix="fs" );
        ~GridFS();

        /**
         * puts the file reference by fileName into the db
         * @param fileName relative to process
         * @return the file object
         */
        BSONObj storeFile( const string& fileName );

        /**
         * removes file referenced by fileName from the db
         * @param fileName relative to process
         * @return the file object
         */
        void removeFile( const string& fileName );

        /**
         * returns a file object matching the query
         */
        GridFile findFile( BSONObj query );

        /**
         * equiv to findFile( { filename : filename } )
         */
        GridFile findFile( const string& fileName );

        /**
         * convenience method to get all the files
         */
        auto_ptr<DBClientCursor> list();

        /**
         * convenience method to get all the files with a filter
         */
        auto_ptr<DBClientCursor> list( BSONObj query );

    private:
        DBClientBase& _client;
        string _dbName;
        string _prefix;
        string _filesNS;
        string _chunksNS;

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
        bool exists(){
            return ! _obj.isEmpty();
        }

        string getFilename(){
            return _obj["filename"].str();
        }

        int getChunkSize(){
            return (int)(_obj["chunkSize"].number());
        }

        gridfs_offset getContentLength(){
            return (gridfs_offset)(_obj["length"].number());
        }

        unsigned long long getUploadDate(){
            return _obj["uploadDate"].date();
        }

        string getMD5(){
            return _obj["md5"].str();
        }

        BSONObj getMetadata();

        int getNumChunks(){
            return (int) ceil( (double)getContentLength() / (double)getChunkSize() );
        }

        Chunk getChunk( int n );

        /**
           write the file to the output stream
         */
        gridfs_offset write( ostream & out );

        /**
           write the file to this filename
         */
        gridfs_offset write( const string& where );

    private:
        GridFile( GridFS * grid , BSONObj obj );

        void _exists();

        GridFS * _grid;
        BSONObj _obj;

        friend class GridFS;
    };
}


