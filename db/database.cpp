// database.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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
*/

#include "pch.h"
#include "pdfile.h"
#include "database.h"
#include "instance.h"

namespace mongo {

    bool Database::_openAllFiles = false;

    Database::~Database() {
        magic = 0;
        btreeStore->closeFiles(name, path);
        size_t n = files.size();
        for ( size_t i = 0; i < n; i++ )
            delete files[i];
        if( ccByLoc.size() ) { 
            log() << "\n\n\nWARNING: ccByLoc not empty on database close! " << ccByLoc.size() << ' ' << name << endl;
        }
    }

    Database::Database(const char *nm, bool& newDb, const string& _path )
        : name(nm), path(_path), namespaceIndex( path, name ), 
          profileName(name + ".system.profile")
    {
        
        { // check db name is valid
            size_t L = strlen(nm);
            uassert( 10028 ,  "db name is empty", L > 0 );
            uassert( 10029 ,  "bad db name [1]", *nm != '.' );
            uassert( 10030 ,  "bad db name [2]", nm[L-1] != '.' );
            uassert( 10031 ,  "bad char(s) in db name", strchr(nm, ' ') == 0 );
            uassert( 10032 ,  "db name too long", L < 64 );
        }
        
        newDb = namespaceIndex.exists();
        profile = 0;

        {
            vector<string> others;
            getDatabaseNames( others , path );
            
            for ( unsigned i=0; i<others.size(); i++ ){

                if ( strcasecmp( others[i].c_str() , nm ) )
                    continue;

                if ( strcmp( others[i].c_str() , nm ) == 0 )
                    continue;
                
                stringstream ss;
                ss << "db already exists with different case other: [" << others[i] << "] me [" << nm << "]";
                uasserted( DatabaseDifferCaseCode , ss.str() );
            }
        }

        
        // If already exists, open.  Otherwise behave as if empty until
        // there's a write, then open.
        if ( ! newDb || cmdLine.defaultProfile ) {
            namespaceIndex.init();
            if( _openAllFiles )
                openAllFiles();
            
        }
       

        magic = 781231;
    }


    bool Database::setProfilingLevel( int newLevel , string& errmsg ){
        if ( profile == newLevel )
            return true;
        
        if ( newLevel < 0 || newLevel > 2 ){
            errmsg = "profiling level has to be >=0 and <= 2";
            return false;
        }
        
        if ( newLevel == 0 ){
            profile = 0;
            return true;
        }
        
        assert( cc().database() == this );

        if ( ! namespaceIndex.details( profileName.c_str() ) ){
            log(1) << "creating profile ns: " << profileName << endl;
            BSONObjBuilder spec;
            spec.appendBool( "capped", true );
            spec.append( "size", 131072.0 );
            if ( ! userCreateNS( profileName.c_str(), spec.done(), errmsg , true ) ){
                return false;
            }
        }
        profile = newLevel;
        return true;
    }

    void Database::finishInit(){
        if ( cmdLine.defaultProfile == profile )
            return;
        
        string errmsg;
        massert( 12506 , errmsg , setProfilingLevel( cmdLine.defaultProfile , errmsg ) );
    }

    bool Database::validDBName( const string& ns ){
        if ( ns.size() == 0 || ns.size() > 64 )
            return false;
        size_t good = strcspn( ns.c_str() , "/\\. \"" );
        return good == ns.size();
    }

    void Database::flushFiles( bool sync ){
        dbMutex.assertAtLeastReadLocked();
        for ( unsigned i=0; i<files.size(); i++ ){
            files[i]->flush( sync );
        }
    }

} // namespace mongo
