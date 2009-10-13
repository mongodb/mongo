// gridfs.cpp

#include "../stdafx.h"
#include <fcntl.h>

#include "gridfs.h"

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

    BSONObj GridFS::storeFile( const string& fileName , const string& remoteName ){
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
            char buf[DEFAULT_CHUNK_SIZE];
            char* bufPos = buf;
            unsigned int chunkLen = 0; // how much in the chunk now
            while(chunkLen != DEFAULT_CHUNK_SIZE && !feof(fd)){
                int readLen = fread(bufPos, 1, DEFAULT_CHUNK_SIZE - chunkLen, fd);
                chunkLen += readLen;
                bufPos += readLen;

                assert(chunkLen <= DEFAULT_CHUNK_SIZE);
            }

            Chunk c(idObj, chunkNumber, buf, chunkLen);
            _client.insert( _chunksNS.c_str() , c._data );

            length += chunkLen;
            chunkNumber++;
        }

        if (fd != stdin)
            fclose( fd );
        
        massert("large files not yet implemented", length <= 0xffffffff);

        BSONObj res;
        if ( ! _client.runCommand( _dbName.c_str() , BSON( "filemd5" << id << "root" << _prefix ) , res ) )
            throw UserException( "filemd5 failed" );

        BSONObj fileObject =
            BSON( "_id" << id
               << "filename" << (remoteName.empty() ? fileName : remoteName)
               << "length" << (unsigned) length
               << "chunkSize" << DEFAULT_CHUNK_SIZE
               << "md5" << res["md5"]
            );

        _client.insert(_filesNS.c_str(), fileObject );

        return fileObject;
    }

    void GridFS::removeFile( const string& fileName ){
        BSONObj idObj = _client.findOne( _filesNS , BSON( "filename" << fileName ) );
        if ( !idObj.isEmpty() ){
            _client.remove( _filesNS.c_str() , BSON( "filename" << fileName ) );
            _client.remove( _chunksNS.c_str() , BSON( "files_id" << idObj["_id"] ) );
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
