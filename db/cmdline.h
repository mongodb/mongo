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

namespace mongo {
    
    /* command line options        
    */
    /* concurrency: OK/READ */
    struct CmdLine { 
        int port;              // --port

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

        enum { 
            DefaultDBPort = 27017,
			ConfigServerPort = 27019,
			ShardServerPort = 27018
        };

        CmdLine() : 
            port(DefaultDBPort), quiet(false), notablescan(false), prealloc(true), smallfiles(false),
            quota(false), quotaFiles(8), cpu(false), oplogSize(0)
        { } 

    };

    extern CmdLine cmdLine;
}
