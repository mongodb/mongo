// dbshell.cpp

#include <v8.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "ShellUtils.h"
#include "MongoJS.h"

#include "mongo.jsh"


void quitNicely( int sig ){
    write_history( ".dbshell" );
    exit(0);
}

int main(int argc, char* argv[]) {
    signal( SIGINT , quitNicely );

    //v8::V8::SetFlagsFromCommandLine(&argc, argv, true);
    
    v8::HandleScope handle_scope;

    v8::Handle<v8::ObjectTemplate> global = v8::ObjectTemplate::New();

    installShellUtils( global );
    installMongoGlobals( global );

    
    v8::Handle<v8::Context> context = v8::Context::New(NULL, global);
    v8::Context::Scope context_scope(context);
    
    { // init mongo code
        v8::HandleScope handle_scope;
        if ( ! ExecuteString( v8::String::New( jsconcatcode ) , v8::String::New( "(mongo init)" ) , false , true ) )
            return -1;
    }
    
    const char * host = "test";
    
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
                << " --nodb don't connect to mongod on startup.  No 'db address' arg expected.\n"
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
        else {
            const char * last = strstr( str , "/" );
            if ( last )
                last++;
            else
                last = str;
            
            if ( ! strstr( last , "." ) ){
                host = str;
                continue;
            }
                
        }
        
        break;
    }

    if ( !nodb ) { // init mongo code
        v8::HandleScope handle_scope;
        string setup = (string)"db = connect( \"" + host + "\")";
        if ( ! ExecuteString( v8::String::New( setup.c_str() ) , v8::String::New( "(connect)" ) , false , true ) ){
            cout << "error connecting!" << endl;
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
        
        if (!ExecuteString(source, file_name, false, true)){
            cout << "error processing: " << file_name << endl;
            return 1;
        }
        
        numFiles++;
    }
    
    if ( numFiles == 0 )
        runShell = true;

    if ( runShell ){
        
        using_history();
        read_history( ".dbshell" );
        
        cout << "type \"help\" for help" << endl;
        
        v8::Handle<v8::Object> shellHelper = context->Global()->Get( v8::String::New( "shellHelper" ) )->ToObject();

        while ( 1 ){
            
            char * line = readline( "> " );
            
            if ( ! line || ( strlen(line) == 4 && strstr( line , "exit" ) ) ){
                cout << "bye" << endl;
                break;
            }
            
            string code = line;
            
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

            
            if ( strlen( line ) )
                add_history( line );
        }
        
        write_history( ".dbshell" );
    }
    
    return 0;
}


