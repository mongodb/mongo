// gridfs.cpp

/*    Copyright 2009 10gen
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

#include "../stdafx.h"
#include <fcntl.h>
#include <utility>

#include "gridfs.h"
#include <boost/smart_ptr.hpp>

#if defined(_WIN32)
#include <io.h>
#endif

#ifndef MIN
#define MIN(a,b) ( (a) < (b) ? (a) : (b) )
#endif

namespace mongo {

    const unsigned DEFAULT_CHUNK_SIZE = 256 * 1024;

    Chunk::Chunk( BSONObj o ){
        _data = o;
    }

    Chunk::Chunk( BSONObj fileObject , int chunkNumber , const char * data , int len ){
        BSONObjBuilder b;
        b.appendAs( fileObject["_id"] , "files_id" );
        b.append( "n" , chunkNumber );
        b.appendBinDataArray( "data" , data , len );
        _data = b.obj();
    }


    GridFS::GridFS( DBClientBase& client , const string& dbName , const string& prefix ) : _client( client ) , _dbName( dbName ) , _prefix( prefix ){
        _filesNS = dbName + "." + prefix + ".files";
        _chunksNS = dbName + "." + prefix + ".chunks";


        client.ensureIndex( _filesNS , BSON( "filename" << 1 ) );
        client.ensureIndex( _chunksNS , BSON( "files_id" << 1 << "n" << 1 ) );
    }

    GridFS::~GridFS(){

    }

    BSONObj GridFS::storeFile( const char* data , size_t length , const string& remoteName , const string& contentType){
        massert("large files not yet implemented", length <= 0xffffffff);
        char const * const end = data + length;

        OID id;
        id.init();
        BSONObj idObj = BSON("_id" << id);

        int chunkNumber = 0;
        while (data < end){
            int chunkLen = MIN(DEFAULT_CHUNK_SIZE, (unsigned)(end-data));
            Chunk c(idObj, chunkNumber, data, chunkLen);
            _client.insert( _chunksNS.c_str() , c._data );

            chunkNumber++;
            data += chunkLen;
        }

        return insertFile(remoteName, id, length, contentType);
    }


    BSONObj GridFS::storeFile( const string& fileName , const string& remoteName , const string& contentType){
        uassert( "file doesn't exist" , fileName == "-" || boost::filesystem::exists( fileName ) );

        FILE* fd;
        if (fileName == "-")
            fd = stdin;
        else
            fd = fopen( fileName.c_str() , "rb" );
        uassert("error opening file", fd);

        OID id;
        id.init();
        BSONObj idObj = BSON("_id" << id);

        int chunkNumber = 0;
        gridfs_offset length = 0;
        while (!feof(fd)){
            boost::scoped_array<char>buf (new char[DEFAULT_CHUNK_SIZE]);
            char* bufPos = buf.get();
            unsigned int chunkLen = 0; // how much in the chunk now
            while(chunkLen != DEFAULT_CHUNK_SIZE && !feof(fd)){
                int readLen = fread(bufPos, 1, DEFAULT_CHUNK_SIZE - chunkLen, fd);
                chunkLen += readLen;
                bufPos += readLen;

                assert(chunkLen <= DEFAULT_CHUNK_SIZE);
            }

            Chunk c(idObj, chunkNumber, buf.get(), chunkLen);
            _client.insert( _chunksNS.c_str() , c._data );

            length += chunkLen;
            chunkNumber++;
        }

        if (fd != stdin)
            fclose( fd );
        
        massert("large files not yet implemented", length <= 0xffffffff);

        return insertFile((remoteName.empty() ? fileName : remoteName), id, length, contentType);
    }

    BSONObj GridFS::insertFile(const string& name, const OID& id, unsigned length, const string& contentType){

        BSONObj res;
        if ( ! _client.runCommand( _dbName.c_str() , BSON( "filemd5" << id << "root" << _prefix ) , res ) )
            throw UserException( "filemd5 failed" );

        BSONObjBuilder file;
        file << "_id" << id
             << "filename" << name
             << "length" << (unsigned) length
             << "chunkSize" << DEFAULT_CHUNK_SIZE
             << "uploadDate" << DATENOW
             << "md5" << res["md5"]
             ;

        if (!contentType.empty())
            file << "contentType" << contentType;

        BSONObj ret = file.obj();
        _client.insert(_filesNS.c_str(), ret);

        return ret;
    }

    void GridFS::removeFile( const string& fileName ){
        auto_ptr<DBClientCursor> files = _client.query( _filesNS , BSON( "filename" << fileName ) );
        while (files->more()){
            BSONObj file = files->next();
            BSONElement id = file["_id"];
            _client.remove( _filesNS.c_str() , BSON( "_id" << id ) );
            _client.remove( _chunksNS.c_str() , BSON( "files_id" << id ) );
        }
    }

    GridFile::GridFile( GridFS * grid , BSONObj obj ){
        _grid = grid;
        _obj = obj;
    }

    GridFile GridFS::findFile( const string& fileName ){
        return findFile( BSON( "filename" << fileName ) );
    };

    GridFile GridFS::findFile( BSONObj query ){
        query = BSON("query" << query << "orderby" << BSON("uploadDate" << -1));
        return GridFile( this , _client.findOne( _filesNS.c_str() , query ) );
    }

    auto_ptr<DBClientCursor> GridFS::list(){
        return _client.query( _filesNS.c_str() , BSONObj() );
    }

    auto_ptr<DBClientCursor> GridFS::list( BSONObj o ){
        return _client.query( _filesNS.c_str() , o );
    }

    BSONObj GridFile::getMetadata(){
        BSONElement meta_element = _obj["metadata"];
        if( meta_element.eoo() ){
            return BSONObj();
        }

        return meta_element.embeddedObject();
    }

    Chunk GridFile::getChunk( int n ){
        _exists();
        BSONObjBuilder b;
        b.appendAs( _obj["_id"] , "files_id" );
        b.append( "n" , n );

        BSONObj o = _grid->_client.findOne( _grid->_chunksNS.c_str() , b.obj() );
        uassert( "chunk is empty!" , ! o.isEmpty() );
        return Chunk(o);
    }

    gridfs_offset GridFile::write( ostream & out ){
        _exists();

        const int num = getNumChunks();

        for ( int i=0; i<num; i++ ){
            Chunk c = getChunk( i );

            int len;
            const char * data = c.data( len );
            out.write( data , len );
        }

        return getContentLength();
    }

    gridfs_offset GridFile::write( const string& where ){
        if (where == "-"){
            return write( cout );
        } else {
            ofstream out(where.c_str() , ios::out | ios::binary );
            return write( out );
        }
    }

    void GridFile::_exists(){
        uassert( "doesn't exists" , exists() );
    }

}
