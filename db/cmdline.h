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

#pragma once

#include "../pch.h"

namespace mongo {
    
    /* command line options        
    */
    /* concurrency: OK/READ */
    struct CmdLine { 
        string binaryName;     // mongod or mongos

        int port;              // --port
        string bind_ip;        // --bind_ip
        bool rest;             // --rest
        bool jsonp;             // --jsonp

        string _replSet;       // --replSet[/<seedlist>]
        string ourSetName() const { 
            string setname;
            size_t sl = _replSet.find('/');
            if( sl == string::npos )
                return _replSet;
            return _replSet.substr(0, sl);
        }

        string source;         // --source
        string only;           // --only
        
        bool quiet;            // --quiet
        bool notablescan;      // --notablescan
        bool prealloc;         // --noprealloc
        bool smallfiles;       // --smallfiles
        
        bool quota;            // --quota
        int quotaFiles;        // --quotaFiles
        bool cpu;              // --cpu show cpu time periodically

        long long oplogSize;   // --oplogSize
        int defaultProfile;    // --profile
        int slowMS;            // --time in ms that is "slow"

        int pretouch;          // --pretouch for replication application (experimental)
        bool moveParanoia;     // for move chunk paranoia 
        
        enum { 
            DefaultDBPort = 27017,
			ConfigServerPort = 27019,
			ShardServerPort = 27018
        };

        CmdLine() : 
            port(DefaultDBPort), rest(false), jsonp(false), quiet(false), notablescan(false), prealloc(true), smallfiles(false),
            quota(false), quotaFiles(8), cpu(false), oplogSize(0), defaultProfile(0), slowMS(100), pretouch(0), moveParanoia( true )
        { } 
        

        static void addGlobalOptions( boost::program_options::options_description& general , 
                                      boost::program_options::options_description& hidden );

        static void addWindowsOptions( boost::program_options::options_description& windows , 
                                      boost::program_options::options_description& hidden );

        
        /**
         * @return true if should run program, false if should exit
         */
        static bool store( int argc , char ** argv , 
                           boost::program_options::options_description& visible,
                           boost::program_options::options_description& hidden,
                           boost::program_options::positional_options_description& positional,
                           boost::program_options::variables_map &output );
    };
    
    extern CmdLine cmdLine;
    
    void setupCoreSignals();
}
