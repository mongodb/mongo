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
#include "jsobj.h"

namespace mongo {
    
    /* command line options        
    */
    /* concurrency: OK/READ */
    struct CmdLine { 
        CmdLine();
        
        string binaryName;     // mongod or mongos

        int port;              // --port
        enum { 
            DefaultDBPort = 27017,
            ConfigServerPort = 27019,
            ShardServerPort = 27018
        };
        bool isDefaultPort() const { return port == DefaultDBPort; }

        string bind_ip;        // --bind_ip
        bool rest;             // --rest
        bool jsonp;            // --jsonp

        string _replSet;       // --replSet[/<seedlist>]
        string ourSetName() const { 
            string setname;
            size_t sl = _replSet.find('/');
            if( sl == string::npos )
                return _replSet;
            return _replSet.substr(0, sl);
        }
        bool usingReplSets() const { return !_replSet.empty(); }

        // for master/slave replication
        string source;         // --source
        string only;           // --only
        
        bool quiet;            // --quiet
        bool noTableScan;      // --notablescan no table scans allowed
        bool prealloc;         // --noprealloc no preallocation of data files
        bool smallfiles;       // --smallfiles allocate smaller data files
        
        bool quota;            // --quota
        int quotaFiles;        // --quotaFiles
        bool cpu;              // --cpu show cpu time periodically

        bool dur;              // --dur durability

        /** --durTrace 7      dump journal and terminate without doing anything further 
            --durTrace 4      recover and terminate without listening
        */
        enum { // bits to be ORed
            DurDumpJournal = 1,   // dump diagnostics on the journal during recovery
            DurScanOnly = 2,      // don't do any real work, just scan and dump if dump specified
            DurRecoverOnly = 4    // terminate after recovery step
        };
        int durTrace;          // --durTrace <n> for debugging

        long long oplogSize;   // --oplogSize
        int defaultProfile;    // --profile
        int slowMS;            // --time in ms that is "slow"

        int pretouch;          // --pretouch for replication application (experimental)
        bool moveParanoia;     // for move chunk paranoia 
        double syncdelay;      // seconds between fsyncs

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

    string prettyHostName();


    /**
     * used for setParameter
     * so you can write validation code that lives with code using it
     * rather than all in the command place
     * also lets you have mongos or mongod specific code
     * without pulling it all sorts of things
     */
    class ParameterValidator {
    public:
        ParameterValidator( const string& name );
        virtual ~ParameterValidator(){}

        virtual bool isValid( BSONElement e , string& errmsg ) = 0;

        static ParameterValidator * get( const string& name );

    private:
        string _name;
        
        // don't need to lock since this is all done in static init
        static map<string,ParameterValidator*> * _all; 
    };
    
}
