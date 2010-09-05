// cmdline.cpp

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
#include "cmdline.h"
#include "commands.h"
#include "../util/processinfo.h"

namespace po = boost::program_options;

namespace mongo {

    void setupSignals();
    BSONArray argvArray;

    void CmdLine::addGlobalOptions( boost::program_options::options_description& general , 
                                    boost::program_options::options_description& hidden ){
        /* support for -vv -vvvv etc. */
        for (string s = "vv"; s.length() <= 12; s.append("v")) {
            hidden.add_options()(s.c_str(), "verbose");
        }
        
        general.add_options()
            ("help,h", "show this usage information")
            ("version", "show version information")
            ("config,f", po::value<string>(), "configuration file specifying additional options")
            ("verbose,v", "be more verbose (include multiple times for more verbosity e.g. -vvvvv)")
            ("quiet", "quieter output")
            ("port", po::value<int>(&cmdLine.port), "specify port number")
            ("bind_ip", po::value<string>(&cmdLine.bind_ip), "comma separated list of ip addresses to listen on - all local ips by default")
            ("logpath", po::value<string>() , "log file to send write to instead of stdout - has to be a file, not directory" )
            ("logappend" , "append to logpath instead of over-writing" )
            ("pidfilepath", po::value<string>(), "full path to pidfile (if not set, no pidfile is created)")
#ifndef _WIN32
            ("fork" , "fork server process" )
#endif
            ;
        
    }


#if defined(_WIN32)
    void CmdLine::addWindowsOptions( boost::program_options::options_description& windows , 
                                    boost::program_options::options_description& hidden ){
        windows.add_options()
            ("install", "install mongodb service")
            ("remove", "remove mongodb service")
            ("reinstall", "reinstall mongodb service (equivilant of mongod --remove followed by mongod --install)")
            ("serviceName", po::value<string>(), "windows service name")
            ("serviceUser", po::value<string>(), "user name service executes as")
            ("servicePassword", po::value<string>(), "password used to authenticate serviceUser")
            ;
        hidden.add_options()("service", "start mongodb service");
    }
#endif


    bool CmdLine::store( int argc , char ** argv , 
                         boost::program_options::options_description& visible,
                         boost::program_options::options_description& hidden,
                         boost::program_options::positional_options_description& positional,
                         boost::program_options::variables_map &params ){
        
        
        /* don't allow guessing - creates ambiguities when some options are
         * prefixes of others. allow long disguises and don't allow guessing
         * to get away with our vvvvvvv trick. */
        int style = (((po::command_line_style::unix_style ^
                       po::command_line_style::allow_guessing) |
                      po::command_line_style::allow_long_disguise) ^
                     po::command_line_style::allow_sticky);

        
        try {

            po::options_description all;
            all.add( visible );
            all.add( hidden );

            po::store( po::command_line_parser(argc, argv)
                       .options( all )
                       .positional( positional )
                       .style( style )
                       .run(), 
                       params );

            if ( params.count("config") ){
                ifstream f( params["config"].as<string>().c_str() );
                if ( ! f.is_open() ){
                    cout << "ERROR: could not read from config file" << endl << endl;
                    cout << visible << endl;
                    return false;
                }
                
                po::store( po::parse_config_file( f , all ) , params );
                f.close();
            }
            
            po::notify(params);
        } 
        catch (po::error &e) {
            cout << "ERROR: " << e.what() << endl << endl;
            cout << visible << endl;
            return false;
        }

        if (params.count("verbose")) {
            logLevel = 1;
        }

        for (string s = "vv"; s.length() <= 12; s.append("v")) {
            if (params.count(s)) {
                logLevel = s.length();
            }
        }

        if (params.count("quiet")) {
            cmdLine.quiet = true;
        }

        string logpath;

#ifndef _WIN32
        if (params.count("fork")) {
            if ( ! params.count( "logpath" ) ){
                cout << "--fork has to be used with --logpath" << endl;
                ::exit(-1);
            }
            
            { // test logpath
                logpath = params["logpath"].as<string>();
                assert( logpath.size() );
                if ( logpath[0] != '/' ){
                    char temp[256];
                    assert( getcwd( temp , 256 ) );
                    logpath = (string)temp + "/" + logpath;
                }
                FILE * test = fopen( logpath.c_str() , "a" );
                if ( ! test ){
                    cout << "can't open [" << logpath << "] for log file: " << errnoWithDescription() << endl;
                    ::exit(-1);
                }
                fclose( test );
            }
            
            cout.flush();
            cerr.flush();

            pid_t c = fork();
            if ( c ){
                _exit(0);
            }

            if ( chdir("/") < 0 ){
                cout << "Cant chdir() while forking server process: " << strerror(errno) << endl;
                ::exit(-1);
            }
            setsid();
            
            pid_t c2 = fork();
            if ( c2 ){
                cout << "forked process: " << c2 << endl;
                _exit(0);
            }

            // stdout handled in initLogging
            //fclose(stdout);
            //freopen("/dev/null", "w", stdout);

            fclose(stderr);
            fclose(stdin);

            FILE* f = freopen("/dev/null", "w", stderr);
            if ( f == NULL ){
                cout << "Cant reassign stderr while forking server process: " << strerror(errno) << endl;
                ::exit(-1);
            }

            f = freopen("/dev/null", "r", stdin);
            if ( f == NULL ){
                cout << "Cant reassign stdin while forking server process: " << strerror(errno) << endl;
                ::exit(-1);
            }

            setupCoreSignals();
            setupSignals();
        }
#endif
        if (params.count("logpath")) {
            if ( logpath.size() == 0 )
                logpath = params["logpath"].as<string>();
            uassert( 10033 ,  "logpath has to be non-zero" , logpath.size() );
            initLogging( logpath , params.count( "logappend" ) );
        }

        if ( params.count("pidfilepath")) {
            writePidFile( params["pidfilepath"].as<string>() );
        }

        {
            BSONArrayBuilder b;
            for (int i=0; i < argc; i++)
                b << argv[i];
            argvArray = b.arr();
        }

        return true;
    }
    
    void ignoreSignal( int signal ){
    }

    void setupCoreSignals(){
#if !defined(_WIN32)
        assert( signal(SIGUSR1 , rotateLogs ) != SIG_ERR );
        assert( signal(SIGHUP , ignoreSignal ) != SIG_ERR );
#endif
    }

    class CmdGetCmdLineOpts : Command{
        public:
        CmdGetCmdLineOpts(): Command("getCmdLineOpts") {}
        void help(stringstream& h) const { h << "get argv"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return true; }
        virtual bool slaveOk() const { return true; }

        virtual bool run(const string&, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl){
            result.append("argv", argvArray);
            return true;
        }

    } cmdGetCmdLineOpts;
}
