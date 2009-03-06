// dbshell.cpp

#include <v8.h>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "ShellUtils.h"
#include "MongoJS.h"

#include "mongo.jsh"


void shellHistoryInit(){
#ifdef USE_READLINE
    using_history();
    read_history( ".dbshell" );
#else
    cout << "type \"exit\" to exit" << endl;
#endif
}
void shellHistoryDone(){
#ifdef USE_READLINE
    write_history( ".dbshell" );
#endif
}
void shellHistoryAdd( const char * line ){
    if ( strlen(line) == 0 )
        return;
#ifdef USE_READLINE
    add_history( line );
#endif
}
char * shellReadline( const char * prompt ){
#ifdef USE_READLINE
  return readline( "> " );
#else
  printf( "> " );
  char * buf = new char[1024];
  char * l = fgets( buf , 1024 , stdin );
  int len = strlen( buf );
  buf[len-1] = 0;
  return l;
#endif
}

void quitNicely( int sig ){
    shellHistoryDone();
    exit(0);
}

void quitAbruptly( int sig ) {
    KillMongoProgramInstances();
    exit(14);    
}

void setupSignals() {
#ifndef _WIN32
    signal( SIGINT , quitNicely );
    signal( SIGTERM , quitNicely );
    signal( SIGPIPE , quitNicely ); // Maybe just log and continue?
    signal( SIGABRT , quitAbruptly );
    signal( SIGSEGV , quitAbruptly );
    signal( SIGBUS , quitAbruptly );
    signal( SIGFPE , quitAbruptly );
#endif
}

string fixHost( string url , string host , string port ){
    if ( host.size() == 0 && port.size() == 0 ){
        if ( url.find( "/" ) == string::npos && url.find( "." ) != string::npos )
            return url + "/test";
        return url;
    }
    
    if ( url.find( "/" ) != string::npos ){
        cerr << "url can't have host or port if you specify them individually" << endl;
        exit(-1);
    }
    
    if ( host.size() == 0 )
        host = "127.0.0.1";

    string newurl = host;
    if ( port.size() > 0 )
        newurl += ":" + port;
    
    newurl += "/" + url;
    
    return newurl;
}

v8::Handle< v8::Context > baseContext_;

int main(int argc, char* argv[]) {
    setupSignals();
    
    RecordMyLocation( argv[ 0 ] );

    //v8::V8::SetFlagsFromCommandLine(&argc, argv, true);
    
    v8::Locker l;
    v8::HandleScope handle_scope;

    v8::Handle<v8::ObjectTemplate> global = v8::ObjectTemplate::New();

    installShellUtils( global );
    installMongoGlobals( global );

    baseContext_ = v8::Context::New(NULL, global);
    v8::Context::Scope context_scope(baseContext_);
    
    { // init mongo code
        v8::HandleScope handle_scope;
        if ( ! ExecuteString( v8::String::New( jsconcatcode ) , v8::String::New( "(mongo init)" ) , false , true ) )
            return -1;
    }
    
    string url = "test";
    string dbhost;
    string port;
    
    string username;
    string password;

    bool runShell = false;
    bool nodb = false;
    
    int argNumber = 1;
    for ( ; argNumber < argc; argNumber++) {
        const char* str = argv[argNumber];
        
        if (strcmp(str, "--shell") == 0) {
            runShell = true;
            continue;
        } 
        
        if (strcmp(str, "--nodb") == 0) {
            nodb = true;
            continue;
        } 

        if ( strcmp( str , "--port" ) == 0 ){
            port = argv[argNumber+1];
            argNumber++;
            continue;
        }

        if ( strcmp( str , "--host" ) == 0 ){
            dbhost = argv[argNumber+1];
            argNumber++;
            continue;
        }


        if ( strcmp( str , "-u" ) == 0 ){
            username = argv[argNumber+1];
            argNumber++;
            continue;
        }

        if ( strcmp( str , "-p" ) == 0 ){
            password = argv[argNumber+1];
            argNumber++;
            continue;
        }

        if ( strstr( str , "-p" ) == str ){
            password = str;
            password = password.substr( 2 );
            continue;
        }

        if ( strcmp(str, "--help") == 0 || 
             strcmp(str, "-h" ) == 0 ) {

            cout 
                << "usage: " << argv[0] << " [options] [db address] [file names]\n" 
                << "db address can be:\n"
                << "   foo   =   foo database on local machine\n" 
                << "   192.169.0.5/foo   =   foo database on 192.168.0.5 machine\n" 
                << "   192.169.0.5:9999/foo   =   foo database on 192.168.0.5 machine on port 9999\n"
                << "options\n"
                << " --shell run the shell after executing files\n"
                << " -u <username>\n"
                << " -p<password> - notice no space\n"
                << " --host <host> - server to connect to\n"
                << " --port <port> - port to connect to\n"
                << " --nodb don't connect to mongo program on startup.  No 'db address' arg expected.\n"
                << "file names: a list of files to run.  will exit after unless --shell is specified\n"
                ;
            
            return 0;
        } 

        if (strcmp(str, "-f") == 0) {
            continue;
        } 
        
        if (strncmp(str, "--", 2) == 0) {
            printf("Warning: unknown flag %s.\nTry --help for options\n", str);
            continue;
        } 
        
        if ( nodb )
            break;

        const char * last = strstr( str , "/" );
        if ( last )
            last++;
        else
            last = str;
        
        if ( ! strstr( last , "." ) ){
            url = str;
            continue;
        }
        
        if ( strstr( last , ".js" ) )
            break;

        path p( str );
        if ( ! boost::filesystem::exists( p ) ){
            url = str;
            continue;
        }
            

        break;
    }

    if ( !nodb ) { // init mongo code
        v8::HandleScope handle_scope;
        cout << "url: " << url << endl;
        string setup = (string)"db = connect( \"" + fixHost( url , dbhost , port ) + "\")";
        if ( ! ExecuteString( v8::String::New( setup.c_str() ) , v8::String::New( "(connect)" ) , false , true ) ){
            return -1;
        }

        if ( username.size() && password.size() ){
            stringstream ss;
            ss << "if ( ! db.auth( \"" << username << "\" , \"" << password << "\" ) ){ throw 'login failed'; }";

            if ( ! ExecuteString( v8::String::New( ss.str().c_str() ) , v8::String::New( "(auth)" ) , true , true ) ){
                cout << "login failed" << endl;
                return -1;
            }
                

        }

    }    
    
    int numFiles = 0;
    
    for ( ; argNumber < argc; argNumber++) {
        const char* str = argv[argNumber];

        v8::HandleScope handle_scope;
        v8::Handle<v8::String> file_name = v8::String::New(str);
        v8::Handle<v8::String> source = ReadFile(str);
        if (source.IsEmpty()) {
            printf("Error reading '%s'\n", str);
            return 1;
        }
        
        MongoProgramScope s;
        if (!ExecuteString(source, file_name, false, true)){
            cout << "error processing: " << file_name << endl;
            return 1;
        }
        
        numFiles++;
    }
    
    if ( numFiles == 0 )
        runShell = true;

    if ( runShell ){
        
        MongoProgramScope s;

        shellHistoryInit();
        
        cout << "type \"help\" for help" << endl;
        
        v8::Handle<v8::Object> shellHelper = baseContext_->Global()->Get( v8::String::New( "shellHelper" ) )->ToObject();

        while ( 1 ){
            
            char * line = shellReadline( "> " );
            
            if ( ! line || ( strlen(line) == 4 && strstr( line , "exit" ) ) ){
                cout << "bye" << endl;
                break;
            }
            
            string code = line;
            if ( code == "exit" ){
	        break;
	    }

            {
                string cmd = line;
                if ( cmd.find( " " ) > 0 )
                    cmd = cmd.substr( 0 , cmd.find( " " ) );
                
                if ( shellHelper->HasRealNamedProperty( v8::String::New( cmd.c_str() ) ) ){
                    stringstream ss;
                    ss << "shellHelper( \"" << cmd << "\" , \"" << code.substr( cmd.size() ) << "\" )";
                    code = ss.str();
                }
                
            }

            v8::HandleScope handle_scope;
            ExecuteString(v8::String::New( code.c_str() ),
                          v8::String::New("(shell)"),
                          true,
                          true);

            
	    shellHistoryAdd( line );
        }
        
	shellHistoryDone();
    }
    
    return 0;
}


