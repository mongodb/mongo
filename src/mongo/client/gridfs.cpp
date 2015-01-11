// gridfs.cpp

/*    Copyright 2009 10gen
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

#include "mongo/platform/basic.h"

#include "mongo/client/gridfs.h"

#include <boost/filesystem/operations.hpp>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#endif

#include "mongo/client/dbclientcursor.h"

#ifndef MIN
#define MIN(a,b) ( (a) < (b) ? (a) : (b) )
#endif


namespace mongo {

    using std::auto_ptr;
    using std::cout;
    using std::endl;
    using std::ios;
    using std::ofstream;
    using std::ostream;
    using std::string;

    const unsigned DEFAULT_CHUNK_SIZE = 255 * 1024;

    GridFSChunk::GridFSChunk( BSONObj o ) {
        _data = o;
    }

    GridFSChunk::GridFSChunk( BSONObj fileObject , int chunkNumber , const char * data , int len ) {
        BSONObjBuilder b;
        b.appendAs( fileObject["_id"] , "files_id" );
        b.append( "n" , chunkNumber );
        b.appendBinData( "data" , len, BinDataGeneral, data );
        _data = b.obj();
    }


    GridFS::GridFS( DBClientBase& client , const string& dbName , const string& prefix ) : _client( client ) , _dbName( dbName ) , _prefix( prefix ) {
        _filesNS = dbName + "." + prefix + ".files";
        _chunksNS = dbName + "." + prefix + ".chunks";
        _chunkSize = DEFAULT_CHUNK_SIZE;

        client.ensureIndex( _filesNS , BSON( "filename" << 1 ) );
        client.ensureIndex( _chunksNS , BSON( "files_id" << 1 << "n" << 1 ) , /*unique=*/true );
    }

    GridFS::~GridFS() {

    }

    void GridFS::setChunkSize(unsigned int size) {
        massert( 13296 , "invalid chunk size is specified", (size != 0 ));
        _chunkSize = size;
    }

    unsigned int GridFS::getChunkSize() const {
        return _chunkSize;
    }

    BSONObj GridFS::storeFile( const char* data , size_t length , const string& remoteName , const string& contentType) {
        char const * const end = data + length;

        OID id;
        id.init();
        BSONObj idObj = BSON("_id" << id);

        int chunkNumber = 0;
        while (data < end) {
            int chunkLen = MIN(_chunkSize, (unsigned)(end-data));
            GridFSChunk c(idObj, chunkNumber, data, chunkLen);
            _client.insert( _chunksNS.c_str() , c._data );

            chunkNumber++;
            data += chunkLen;
        }

        return insertFile(remoteName, id, length, contentType);
    }


    BSONObj GridFS::storeFile( const string& fileName , const string& remoteName , const string& contentType) {
        uassert( 10012 ,  "file doesn't exist" , fileName == "-" || boost::filesystem::exists( fileName ) );

        FILE* fd;
        if (fileName == "-")
            fd = stdin;
        else
            fd = fopen( fileName.c_str() , "rb" );
        uassert( 10013 , "error opening file", fd);

        OID id;
        id.init();
        BSONObj idObj = BSON("_id" << id);

        int chunkNumber = 0;
        gridfs_offset length = 0;
        while (!feof(fd)) {
            //boost::scoped_array<char>buf (new char[_chunkSize+1]);
            char * buf = new char[_chunkSize+1];
            char* bufPos = buf;//.get();
            unsigned int chunkLen = 0; // how much in the chunk now
            while(chunkLen != _chunkSize && !feof(fd)) {
                int readLen = fread(bufPos, 1, _chunkSize - chunkLen, fd);
                chunkLen += readLen;
                bufPos += readLen;

                verify(chunkLen <= _chunkSize);
            }

            GridFSChunk c(idObj, chunkNumber, buf, chunkLen);
            _client.insert( _chunksNS.c_str() , c._data );

            length += chunkLen;
            chunkNumber++;
            delete[] buf;
        }

        if (fd != stdin)
            fclose( fd );

        return insertFile((remoteName.empty() ? fileName : remoteName), id, length, contentType);
    }

    BSONObj GridFS::insertFile(const string& name, const OID& id, gridfs_offset length, const string& contentType) {
        // Wait for any pending writebacks to finish
        BSONObj errObj = _client.getLastErrorDetailed();
        uassert( 16428,
                 str::stream() << "Error storing GridFS chunk for file: " << name
                               << ", error: " << errObj,
                 DBClientWithCommands::getLastErrorString(errObj) == "" );

        BSONObj res;
        if ( ! _client.runCommand( _dbName.c_str() , BSON( "filemd5" << id << "root" << _prefix ) , res ) )
            throw UserException( 9008 , "filemd5 failed" );

        BSONObjBuilder file;
        file << "_id" << id
             << "filename" << name
             << "chunkSize" << _chunkSize
             << "uploadDate" << DATENOW
             << "md5" << res["md5"]
             ;

        if (length < 1024*1024*1024) { // 2^30
            file << "length" << (int) length;
        }
        else {
            file << "length" << (long long) length;
        }

        if (!contentType.empty())
            file << "contentType" << contentType;

        BSONObj ret = file.obj();
        _client.insert(_filesNS.c_str(), ret);

        return ret;
    }

    void GridFS::removeFile( const string& fileName ) {
        auto_ptr<DBClientCursor> files = _client.query( _filesNS , BSON( "filename" << fileName ) );
        while (files->more()) {
            BSONObj file = files->next();
            BSONElement id = file["_id"];
            _client.remove( _filesNS.c_str() , BSON( "_id" << id ) );
            _client.remove( _chunksNS.c_str() , BSON( "files_id" << id ) );
        }
    }

    GridFile::GridFile(const GridFS * grid , BSONObj obj ) {
        _grid = grid;
        _obj = obj;
    }

    GridFile GridFS::findFile( const string& fileName ) const {
        return findFile( BSON( "filename" << fileName ) );
    };

    GridFile GridFS::findFile( BSONObj query ) const {
        query = BSON("query" << query << "orderby" << BSON("uploadDate" << -1));
        return GridFile( this , _client.findOne( _filesNS.c_str() , query ) );
    }

    auto_ptr<DBClientCursor> GridFS::list() const {
        return _client.query( _filesNS.c_str() , BSONObj() );
    }

    auto_ptr<DBClientCursor> GridFS::list( BSONObj o ) const {
        return _client.query( _filesNS.c_str() , o );
    }

    BSONObj GridFile::getMetadata() const {
        BSONElement meta_element = _obj["metadata"];
        if( meta_element.eoo() ) {
            return BSONObj();
        }

        return meta_element.embeddedObject();
    }

    GridFSChunk GridFile::getChunk( int n ) const {
        _exists();
        BSONObjBuilder b;
        b.appendAs( _obj["_id"] , "files_id" );
        b.append( "n" , n );

        BSONObj o = _grid->_client.findOne( _grid->_chunksNS.c_str() , b.obj() );
        uassert( 10014 ,  "chunk is empty!" , ! o.isEmpty() );
        return GridFSChunk(o);
    }

    gridfs_offset GridFile::write( ostream & out ) const {
        _exists();

        const int num = getNumChunks();

        for ( int i=0; i<num; i++ ) {
            GridFSChunk c = getChunk( i );

            int len;
            const char * data = c.data( len );
            out.write( data , len );
        }

        return getContentLength();
    }

    gridfs_offset GridFile::write( const string& where ) const {
        if (where == "-") {
            return write( cout );
        }
        else {
            ofstream out(where.c_str() , ios::out | ios::binary );
            uassert(13325, "couldn't open file: " + where, out.is_open() );
            return write( out );
        }
    }

    void GridFile::_exists() const {
        uassert( 10015 ,  "doesn't exists" , exists() );
    }

}
