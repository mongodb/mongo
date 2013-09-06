// extent_manager.h

/**
*    Copyright (C) 2013 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/diskloc.h"

namespace mongo {

    class DataFile;

    /**
     * ExtentManager basics
     *  - one per database
     *  - responsible for managing <db>.# files
     *  - NOT responsible for .ns file
     *  - gives out extents
     *  - responsible for figuring out how to get a new extent
     *  - can use any method it wants to do so
     *  - this structure is NOT stored on disk
     *  - this class is NOT thread safe, locking should be above (for now)
     *
     * implementation:
     *  - ExtentManager holds a list of DataFile
     */
    class ExtentManager {
        MONGO_DISALLOW_COPYING( ExtentManager );

    public:
        ExtentManager( const StringData& dbname, const StringData& path, bool directoryPerDB );
        ~ExtentManager();

        /**
         * deletes all state and puts back to original state
         */
        void reset();

        /**
         * opens all current files
         */
        Status init();

        size_t numFiles() const;
        long long fileSize() const;

        DataFile* getFile( int n, int sizeNeeded = 0, bool preallocateOnly = false );

        DataFile* addAFile( int sizeNeeded, bool preallocateNextFile );

        void preallocateAFile() { getFile( numFiles() , 0, true ); }// XXX-ERH

        void flushFiles( bool sync );

        /* allocate a new Extent
           @param capped - true if capped collection
        */
        Extent* createExtent( const char *ns, int approxSize, bool newCapped, bool enforceQuota );

        /**
         * @param loc - has to be for a specific Record
         */
        Record* recordFor( const DiskLoc& loc ) const;

        /**
         * @param loc - has to be for a specific Record (not an Extent)
         */
        Extent* extentFor( const DiskLoc& loc ) const;

        /**
         * @param loc - has to be for a specific Extent
         */
        Extent* getExtent( const DiskLoc& loc, bool doSanityCheck = true ) const;

        Extent* getNextExtent( Extent* ) const;
        Extent* getPrevExtent( Extent* ) const;

        // get(Next|Prev)Record follows the Record linked list
        // these WILL cross Extent boundaries
        // * @param loc - has to be the DiskLoc for a Record

        DiskLoc getNextRecord( const DiskLoc& loc ) const;

        DiskLoc getPrevRecord( const DiskLoc& loc ) const;

        // does NOT traverse extent boundaries

        DiskLoc getNextRecordInExtent( const DiskLoc& loc ) const;

        DiskLoc getPrevRecordInExtent( const DiskLoc& loc ) const;

        /**
         * quantizes extent size to >= min + page boundary
         */
        static int quantizeExtentSize( int size );

    private:

        const DataFile* _getOpenFile( int n ) const;

        Extent* _createExtentInFile( int fileNo, DataFile* f,
                                     const char* ns, int size, bool newCapped,
                                     bool enforceQuota );

        boost::filesystem::path fileName( int n ) const;

// -----

        std::string _dbname; // i.e. "test"
        std::string _path; // i.e. "/data/db"
        bool _directoryPerDB;

        // must be in the dbLock when touching this (and write locked when writing to of course)
        // however during Database object construction we aren't, which is ok as it isn't yet visible
        //   to others and we are in the dbholder lock then.
        std::vector<DataFile*> _files;

    };

}
