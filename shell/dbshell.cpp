// dbshell.cpp

#include <stdio.h>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#include <setjmp.h>
jmp_buf jbuf;
#endif

#include "../scripting/engine.h"
#include "../client/dbclient.h"
#include "../util/unittest.h"
#include "utils.h"

extern const char * jsconcatcode;

string historyFile;
bool gotInterrupted = 0;
bool inMultiLine = 0;

void shellHistoryInit(){
#ifdef USE_READLINE
    stringstream ss;
    char * h = getenv( "HOME" );
    if ( h )
        ss << h << "/";
    ss << ".dbshell";
    historyFile = ss.str();

    using_history();
    read_history( historyFile.c_str() );

#else
    cout << "type \"exit\" to exit" << endl;
#endif
}
void shellHistoryDone(){
#ifdef USE_READLINE
    write_history( historyFile.c_str() );
#endif
}
void shellHistoryAdd( const char * line ){
    if ( strlen(line) == 0 )
        return;
#ifdef USE_READLINE
    add_history( line );
#endif
}

void intr( int sig ){
#ifdef USE_READLINE
    longjmp( jbuf , 1 );
#endif
}

#if !defined(_WIN32)
void quitNicely( int sig ){
    if ( sig == SIGINT && inMultiLine ){
        gotInterrupted = 1;
        return;
    }
    if ( sig == SIGPIPE )
        mongo::rawOut( "mongo got signal SIGPIPE\n" );
    shellHistoryDone();
    exit(0);
}
#endif

char * shellReadline( const char * prompt , int handlesigint = 0 ){
#ifdef USE_READLINE
    if ( ! handlesigint )
        return readline( prompt );
    if ( setjmp( jbuf ) ){
        gotInterrupted = 1;
        sigrelse(SIGINT);
        signal( SIGINT , quitNicely );
        return 0;
    }
    signal( SIGINT , intr );
    char * ret = readline( prompt );
    signal( SIGINT , quitNicely );
    return ret;
#else
    printf( prompt );
    char * buf = new char[1024];
    char * l = fgets( buf , 1024 , stdin );
    int len = strlen( buf );
    buf[len-1] = 0;
    return l;
#endif
}

#if !defined(_WIN32)
#include <string.h>

void quitAbruptly( int sig ) {
    ostringstream ossSig;
    ossSig << "mongo got signal " << sig << " (" << strsignal( sig ) << "), stack trace: " << endl;
    mongo::rawOut( ossSig.str() );

    ostringstream ossBt;
    mongo::printStackTrace( ossBt );
    mongo::rawOut( ossBt.str() );

    mongo::shellUtils::KillMongoProgramInstances();
    exit(14);
}

void setupSignals() {
    signal( SIGINT , quitNicely );
    signal( SIGTERM , quitNicely );
    signal( SIGPIPE , quitNicely ); // Maybe just log and continue?
    signal( SIGABRT , quitAbruptly );
    signal( SIGSEGV , quitAbruptly );
    signal( SIGBUS , quitAbruptly );
    signal( SIGFPE , quitAbruptly );
}
#else
inline void setupSignals() {}
#endif

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

bool isBalanced( string code ){
    int brackets = 0;
    int parens = 0;

    for ( size_t i=0; i<code.size(); i++ ){
        switch( code[i] ){
        case '/':
            if ( i+1 < code.size() && code[i+1] == '/' ){
                while ( i<code.size() && code[i] != '\n' )
                    i++;
            }
            continue;
        case '{': brackets++; break;
        case '}': brackets--; break;
        case '(': parens++; break;
        case ')': parens--; break;
        case '"':
            i++;
            while ( i < code.size() && code[i] != '"' ) i++;
            break;
        case '\'':
            i++;
            while ( i < code.size() && code[i] != '\'' ) i++;
            break;
        }
    }

    return brackets == 0 && parens == 0;
}

using mongo::asserted;

struct BalancedTest : public mongo::UnitTest {
public:
    void run(){
        assert( isBalanced( "x = 5" ) );
        assert( isBalanced( "function(){}" ) );
        assert( isBalanced( "function(){\n}" ) );
        assert( ! isBalanced( "function(){" ) );
        assert( isBalanced( "x = \"{\";" ) );
        assert( isBalanced( "// {" ) );
        assert( ! isBalanced( "// \n {" ) );
        assert( ! isBalanced( "\"//\" {" ) );

    }
} balnaced_test;

string finishCode( string code ){
    while ( ! isBalanced( code ) ){
        inMultiLine = 1;
        code += "\n";
        char * line = shellReadline("... " , 1 );
        if ( gotInterrupted )
            return "";
        if ( ! line )
            return "";
        code += line;
    }
    return code;
}

#include <boost/program_options.hpp>
namespace po = boost::program_options;

void show_help_text(const char* name, po::options_description options) {
    cout << "usage: " << name << " [options] [db address] [file names (ending in .js)]" << endl
         << "db address can be:" << endl
         << "  foo                   foo database on local machine" << endl
         << "  192.169.0.5/foo       foo database on 192.168.0.5 machine" << endl
         << "  192.169.0.5:9999/foo  foo database on 192.168.0.5 machine on port 9999" << endl
         << options << endl
         << "file names: a list of files to run. files have to end in .js and will exit after "
         << "unless --shell is specified" << endl;
};

bool fileExists( string file ){
    try {
        path p(file);
        return boost::filesystem::exists( file );
    }
    catch (...){
        return false;
    }
}

int _main(int argc, char* argv[]) {
    setupSignals();

    mongo::shellUtils::RecordMyLocation( argv[ 0 ] );

    mongo::ScriptEngine::setup();
    auto_ptr< mongo::Scope > scope( mongo::globalScriptEngine->createScope() );

    string url = "test";
    string dbhost;
    string port;
    vector<string> files;

    string username;
    string password;

    bool runShell = false;
    bool nodb = false;

    string script;

    po::options_description shell_options("options");
    po::options_description hidden_options("Hidden options");
    po::options_description cmdline_options("Command line options");
    po::positional_options_description positional_options;

    shell_options.add_options()
        ("shell", "run the shell after executing files")
        ("nodb", "don't connect to mongod on startup - no 'db address' arg expected")
        ("port", po::value<string>(&port), "port to connect to")
        ("host", po::value<string>(&dbhost), "server to connect to")
        ("eval", po::value<string>(&script), "evaluate javascript")
        ("username,u", po::value<string>(&username), "username for authentication")
        ("password,p", po::value<string>(&password), "password for authentication")
        ("help,h", "show this usage information")
        ;

    hidden_options.add_options()
        ("dbaddress", po::value<string>(), "dbaddress")
        ("files", po::value< vector<string> >(), "files")
        ;

    positional_options.add("dbaddress", 1);
    positional_options.add("files", -1);

    cmdline_options.add(shell_options).add(hidden_options);

    po::variables_map params;

    /* using the same style as db.cpp uses because eventually we're going
     * to merge some of this stuff. */
    int command_line_style = (((po::command_line_style::unix_style ^
                                po::command_line_style::allow_guessing) |
                               po::command_line_style::allow_long_disguise) ^
                              po::command_line_style::allow_sticky);

    try {
        po::store(po::command_line_parser(argc, argv).options(cmdline_options).
                  positional(positional_options).
                  style(command_line_style).run(), params);
        po::notify(params);
    } catch (po::error &e) {
        cout << "ERROR: " << e.what() << endl << endl;
        show_help_text(argv[0], shell_options);
        return mongo::EXIT_BADOPTIONS;
    }

    if (params.count("shell")) {
        runShell = true;
    }
    if (params.count("nodb")) {
        nodb = true;
    }
    if (params.count("help")) {
        show_help_text(argv[0], shell_options);
        return mongo::EXIT_CLEAN;
    }
    if (params.count("files")) {
        files = params["files"].as< vector<string> >();
    }
    /* This is a bit confusing, here are the rules:
     *
     * if nodb is set then all positional parameters are files
     * otherwise the first positional parameter might be a dbaddress, but
     * only if one of these conditions is met:
     *   - it contains no '.' after the last appearance of '\' or '/'
     *   - it doesn't end in '.js' and it doesn't specify a path to an existing file */
    if (params.count("dbaddress")) {
        string dbaddress = params["dbaddress"].as<string>();
        if (nodb) {
            files.insert(files.begin(), dbaddress);
        } else {
            string basename = dbaddress.substr(dbaddress.find_last_of("/\\") + 1);
            if (basename.find_first_of('.') == string::npos ||
                (basename.find(".js", basename.size() - 3) == string::npos && !fileExists(dbaddress))) {
                url = dbaddress;
            } else {
                files.insert(files.begin(), dbaddress);
            }
        }
    }

    scope->externalSetup();
    mongo::shellUtils::installShellUtils( *scope );

    cout << "MongoDB shell version: " << mongo::versionString << endl;

    mongo::UnitTest::runTests();

    if ( !nodb ) { // connect to db
        cout << "url: " << url << endl;
        string setup = (string)"db = connect( \"" + fixHost( url , dbhost , port ) + "\")";
        if ( ! scope->exec( setup , "(connect)" , false , true , false ) )
            return -1;

        if ( username.size() && password.size() ){
            stringstream ss;
            ss << "if ( ! db.auth( \"" << username << "\" , \"" << password << "\" ) ){ throw 'login failed'; }";

            if ( ! scope->exec( ss.str() , "(auth)" , true , true , false ) ){
                cout << "login failed" << endl;
                return -1;
            }

        }

    }

    if ( !script.empty() ) {
        mongo::shellUtils::MongoProgramScope s;
        if ( ! scope->exec( script , "(shell eval)" , true , true , false ) )
            return -4;
    }

    for (size_t i = 0; i < files.size(); i++) {
        mongo::shellUtils::MongoProgramScope s;

        if ( files.size() > 1 )
            cout << "loading file: " << files[i] << endl;

        if ( ! scope->execFile( files[i] , false , true , false ) ){
            cout << "failed to load: " << files[i] << endl;
            return -3;
        }
    }

    if ( files.size() == 0 && script.empty() ) {
        runShell = true;
    }

    if ( runShell ){

        mongo::shellUtils::MongoProgramScope s;

        shellHistoryInit();

        cout << "type \"help\" for help" << endl;

        //v8::Handle<v8::Object> shellHelper = baseContext_->Global()->Get( v8::String::New( "shellHelper" ) )->ToObject();

        while ( 1 ){
            inMultiLine = 0;
            gotInterrupted = 0;
            char * line = shellReadline( "> " );

            if ( line )
                while ( line[0] == ' ' )
                    line++;

            if ( ! line || ( strlen(line) == 4 && strstr( line , "exit" ) ) ){
                cout << "bye" << endl;
                break;
            }

            string code = line;
            if ( code == "exit" ){
                break;
            }
            if ( code.size() == 0 )
                continue;

            code = finishCode( code );
            if ( gotInterrupted ){
                cout << endl;
                continue;
            }

            if ( code.size() == 0 )
                break;

            bool wascmd = false;
            {
                string cmd = line;
                if ( cmd.find( " " ) > 0 )
                    cmd = cmd.substr( 0 , cmd.find( " " ) );

                if ( cmd.find( "\"" ) == string::npos ){
                    scope->exec( (string)"__iscmd__ = shellHelper[\"" + cmd + "\"];" , "(shellhelp1)" , false , true , true );
                    if ( scope->getBoolean( "__iscmd__" )  ){
                        scope->exec( (string)"shellHelper( \"" + cmd + "\" , \"" + code.substr( cmd.size() ) + "\");" , "(shellhelp2)" , false , true , false );
                        wascmd = true;
                    }
                }

            }

            if ( ! wascmd ){
                scope->exec( code.c_str() , "(shell)" , false , true , false );
                scope->exec( "shellPrintHelper( __lastres__ );" , "(shell2)" , true , true , false );
            }


            shellHistoryAdd( line );
        }

        shellHistoryDone();
    }

    return 0;
}

int main(int argc, char* argv[]) {
    try {
        return _main( argc , argv );
    }
    catch ( mongo::DBException& e ){
        cerr << "exception: " << e.what() << endl;
    }
}

namespace mongo {
    DBClientBase * createDirectClient(){
        uassert( "no createDirectClient in shell" , 0 );
        return 0;
    }
}

