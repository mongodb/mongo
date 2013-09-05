// dbshell.cpp
/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/pch.h"

#include <boost/filesystem/operations.hpp>
#include <fstream>
#include <pcrecpp.h>
#include <stdio.h>
#include <string.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/repl/rs_member.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/linenoise.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/util/file.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/password.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

#ifdef _WIN32
#include <io.h>
#include <shlobj.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace std;
using namespace mongo;

string historyFile;
bool gotInterrupted = false;
bool inMultiLine = false;
static volatile bool atPrompt = false; // can eval before getting to prompt
bool autoKillOp = false;

#if !defined(__freebsd__) && !defined(__openbsd__) && !defined(_WIN32)
// this is for ctrl-c handling
#include <setjmp.h>
jmp_buf jbuf;
#endif

namespace mongo {

    Scope * shellMainScope;

    extern bool dbexitCalled;
}

void generateCompletions( const string& prefix , vector<string>& all ) {
    if ( prefix.find( '"' ) != string::npos )
        return;

    try {
        BSONObj args = BSON( "0" << prefix );
        shellMainScope->invokeSafe( "function callShellAutocomplete(x) {shellAutocomplete(x)}", &args, 0, 1000 );
        BSONObjBuilder b;
        shellMainScope->append( b , "" , "__autocomplete__" );
        BSONObj res = b.obj();
        BSONObj arr = res.firstElement().Obj();

        BSONObjIterator i( arr );
        while ( i.more() ) {
            BSONElement e = i.next();
            all.push_back( e.String() );
        }
    }
    catch ( ... ) {
    }
}

void completionHook( const char* text , linenoiseCompletions* lc ) {
    vector<string> all;
    generateCompletions( text , all );

    for ( unsigned i = 0; i < all.size(); ++i )
        linenoiseAddCompletion( lc , (char*)all[i].c_str() );
}

void shellHistoryInit() {
    stringstream ss;
    const char * h = shell_utils::getUserDir();
    if ( h )
        ss << h << "/";
    ss << ".dbshell";
    historyFile = ss.str();

    linenoiseHistoryLoad( historyFile.c_str() );
    linenoiseSetCompletionCallback( completionHook );
}

void shellHistoryDone() {
    linenoiseHistorySave( historyFile.c_str() );
    linenoiseHistoryFree();
}
void shellHistoryAdd( const char * line ) {
    if ( line[0] == '\0' )
        return;

    // dont record duplicate lines
    static string lastLine;
    if ( lastLine == line )
        return;
    lastLine = line;

    // We don't want any .auth() or .addUser() shell helpers added, but we want to
    // be able to add things like `.author`, so be smart about how this is
    // detected by using regular expresions.
    static pcrecpp::RE hiddenHelpers(
            "\\.(auth|addUser|updateUser|changeUserPassword)\\s*\\(");
    // Also don't want the raw user management commands to show in the shell when run directly
    // via runCommand.
    static pcrecpp::RE hiddenCommands(
                "(run|admin)Command\\s*\\(\\s*{\\s*(createUser|updateUser)\\s*:");
    if (!hiddenHelpers.PartialMatch(line) && !hiddenCommands.PartialMatch(line))
    {
        linenoiseHistoryAdd( line );
    }
}

#ifdef CTRLC_HANDLE
void intr( int sig ) {
    longjmp( jbuf , 1 );
}
#endif

void killOps() {
    if ( mongo::shell_utils::_nokillop )
        return;

    if ( atPrompt )
        return;

    sleepmillis(10); // give current op a chance to finish

    mongo::shell_utils::connectionRegistry.killOperationsOnAllConnections( !autoKillOp );
}

void quitNicely( int sig ) {
    mongo::dbexitCalled = true;
    if ( sig == SIGINT && inMultiLine ) {
        gotInterrupted = 1;
        return;
    }

    killOps();
    shellHistoryDone();
    ::_exit(0);
}

// the returned string is allocated with strdup() or malloc() and must be freed by calling free()
char * shellReadline( const char * prompt , int handlesigint = 0 ) {
    atPrompt = true;

#ifdef CTRLC_HANDLE
    if ( ! handlesigint ) {
        char* ret = linenoise( prompt );
        atPrompt = false;
        return ret;
    }
    if ( setjmp( jbuf ) ) {
        gotInterrupted = 1;
        sigrelse(SIGINT);
        signal( SIGINT , quitNicely );
        return 0;
    }
    signal( SIGINT , intr );
#endif

    char * ret = linenoise( prompt );
    if ( ! ret ) {
        gotInterrupted = true;  // got ^C, break out of multiline
    }

    signal( SIGINT , quitNicely );
    atPrompt = false;
    return ret;
}

#ifdef _WIN32
char * strsignal(int sig){
    switch (sig){
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGABRT: return "SIGABRT";
        case SIGSEGV: return "SIGSEGV";
        case SIGFPE: return "SIGFPE";
        default: return "unknown";
    }
}
#endif

void quitAbruptly( int sig ) {
    ostringstream ossSig;
    ossSig << "mongo got signal " << sig << " (" << strsignal( sig ) << "), stack trace: " << endl;
    mongo::rawOut( ossSig.str() );

    ostringstream ossBt;
    mongo::printStackTrace( ossBt );
    mongo::rawOut( ossBt.str() );

    mongo::shell_utils::KillMongoProgramInstances();
    ::_exit( 14 );
}

// this will be called in certain c++ error cases, for example if there are two active
// exceptions
void myterminate() {
    mongo::rawOut( "terminate() called in shell, printing stack:" );
    mongo::printStackTrace();
    ::_exit( 14 );
}

static void ignoreSignal(int ignored) {}

void setupSignals() {
    signal( SIGINT , quitNicely );
    signal( SIGTERM , quitNicely );
    signal( SIGABRT , quitAbruptly );
    signal( SIGSEGV , quitAbruptly );
    signal( SIGFPE , quitAbruptly );

#if !defined(_WIN32) // surprisingly these are the only ones that don't work on windows
    struct sigaction sigactionSignals;
    sigactionSignals.sa_handler = ignoreSignal;
    sigemptyset(&sigactionSignals.sa_mask);
    sigactionSignals.sa_flags = 0;
    sigaction(SIGPIPE, &sigactionSignals, NULL); // errors are handled in socket code directly

    signal( SIGBUS , quitAbruptly );
#endif

    set_terminate( myterminate );
}

string fixHost( const std::string& url, const std::string& host, const std::string& port ) {
    //cout << "fixHost url: " << url << " host: " << host << " port: " << port << endl;

    if ( host.size() == 0 && port.size() == 0 ) {
        if ( url.find( "/" ) == string::npos ) {
            // check for ips
            if ( url.find( "." ) != string::npos )
                return url + "/test";

            if ( url.rfind( ":" ) != string::npos &&
                    isdigit( url[url.rfind(":")+1] ) )
                return url + "/test";
        }
        return url;
    }

    if ( url.find( "/" ) != string::npos ) {
        cerr << "url can't have host or port if you specify them individually" << endl;
        ::_exit(-1);
    }

    string newurl( ( host.size() == 0 ) ? "127.0.0.1" : host );
    if ( port.size() > 0 )
        newurl += ":" + port;
    else if ( host.find(':') == string::npos ) {
        // need to add port with IPv6 addresses
        newurl += ":27017";
    }

    newurl += "/" + url;

    return newurl;
}

static string OpSymbols = "~!%^&*-+=|:,<>/?.";

bool isOpSymbol( char c ) {
    for ( size_t i = 0; i < OpSymbols.size(); i++ )
        if ( OpSymbols[i] == c ) return true;
    return false;
}

bool isUseCmd( const std::string& code ) {
    string cmd = code;
    if ( cmd.find( " " ) > 0 )
        cmd = cmd.substr( 0 , cmd.find( " " ) );
    return cmd == "use";
}

/**
 * Skip over a quoted string, including quotes escaped with backslash
 * 
 * @param code      String
 * @param start     Starting position within string, always > 0
 * @param quote     Quote character (single or double quote)
 * @return          Position of ending quote, or code.size() if no quote found
 */
size_t skipOverString(const std::string& code, size_t start, char quote) {
    size_t pos = start;
    while (pos < code.size()) {
        pos = code.find(quote, pos);
        if (pos == std::string::npos) {
            return code.size();
        }
        // We want to break if the quote we found is not escaped, but we need to make sure
        // that the escaping backslash is not itself escaped.  Comparisons of start and pos
        // are to keep us from reading beyond the beginning of the quoted string.
        //
        if (start == pos     || code[pos - 1] != '\\' || // previous char was backslash
            start == pos - 1 || code[pos - 2] == '\\'    // char before backslash was not another
        ) {
            break;  // The quote we found was not preceded by an unescaped backslash; it is real
        }
        ++pos;      // The quote we found was escaped with backslash, so it doesn't count
    }
    return pos;
}

bool isBalanced( const std::string& code ) {
    if (isUseCmd( code ))
        return true;  // don't balance "use <dbname>" in case dbname contains special chars
    int curlyBrackets = 0;
    int squareBrackets = 0;
    int parens = 0;
    bool danglingOp = false;

    for ( size_t i=0; i<code.size(); i++ ) {
        switch( code[i] ) {
        case '/':
            if ( i + 1 < code.size() && code[i+1] == '/' ) {
                while ( i  <code.size() && code[i] != '\n' )
                    i++;
            }
            continue;
        case '{':
            curlyBrackets++;
            break;
        case '}':
            if ( curlyBrackets <= 0 )
                return true;
            curlyBrackets--;
            break;
        case '[':
            squareBrackets++;
            break;
        case ']':
            if ( squareBrackets <= 0 )
                return true;
            squareBrackets--;
            break;
        case '(':
            parens++;
            break;
        case ')':
            if ( parens <= 0 )
                return true;
            parens--;
            break;
        case '"':
        case '\'':
            i = skipOverString(code, i + 1, code[i]);
            if (i >= code.size()) {
                return true;            // Do not let unterminated strings enter multi-line mode
            }
            break;
        case '\\':
            if ( i + 1 < code.size() && code[i+1] == '/' ) i++;
            break;
        case '+':
        case '-':
            if ( i + 1 < code.size() && code[i+1] == code[i] ) {
                i++;
                continue; // postfix op (++/--) can't be a dangling op
            }
            break;
        }
        if ( i >= code.size() ) {
            danglingOp = false;
            break;
        }
        if ( isOpSymbol( code[i] ) ) danglingOp = true;
        else if ( !std::isspace( static_cast<unsigned char>( code[i] ) ) ) danglingOp = false;
    }

    return curlyBrackets == 0 && squareBrackets == 0 && parens == 0 && !danglingOp;
}

struct BalancedTest : public mongo::StartupTest {
public:
    void run() {
        verify( isBalanced( "x = 5" ) );
        verify( isBalanced( "function(){}" ) );
        verify( isBalanced( "function(){\n}" ) );
        verify( ! isBalanced( "function(){" ) );
        verify( isBalanced( "x = \"{\";" ) );
        verify( isBalanced( "// {" ) );
        verify( ! isBalanced( "// \n {" ) );
        verify( ! isBalanced( "\"//\" {" ) );
        verify( isBalanced( "{x:/x\\//}" ) );
        verify( ! isBalanced( "{ \\/// }" ) );
        verify( isBalanced( "x = 5 + y " ) );
        verify( ! isBalanced( "x = " ) );
        verify( ! isBalanced( "x = // hello" ) );
        verify( ! isBalanced( "x = 5 +" ) );
        verify( isBalanced( " x ++" ) );
        verify( isBalanced( "-- x" ) );
        verify( !isBalanced( "a." ) );
        verify( !isBalanced( "a. " ) );
        verify( isBalanced( "a.b" ) );

        // SERVER-5809 and related cases -- 
        verify( isBalanced( "a = {s:\"\\\"\"}" ) );             // a = {s:"\""}
        verify( isBalanced( "db.test.save({s:\"\\\"\"})" ) );   // db.test.save({s:"\""})
        verify( isBalanced( "printjson(\" \\\" \")" ) );        // printjson(" \" ") -- SERVER-8554
        verify( isBalanced( "var a = \"\\\\\";" ) );            // var a = "\\";
        verify( isBalanced( "var a = (\"\\\\\") //\"" ) );      // var a = ("\\") //"
        verify( isBalanced( "var a = (\"\\\\\") //\\\"" ) );    // var a = ("\\") //\"
        verify( isBalanced( "var a = (\"\\\\\") //" ) );        // var a = ("\\") //
        verify( isBalanced( "var a = (\"\\\\\")" ) );           // var a = ("\\")
        verify( isBalanced( "var a = (\"\\\\\\\"\")" ) );       // var a = ("\\\"")
        verify( ! isBalanced( "var a = (\"\\\\\" //\"" ) );     // var a = ("\\" //"
        verify( ! isBalanced( "var a = (\"\\\\\" //" ) );       // var a = ("\\" //
        verify( ! isBalanced( "var a = (\"\\\\\"" ) );          // var a = ("\\"
    }
} balanced_test;

string finishCode( string code ) {
    while ( ! isBalanced( code ) ) {
        inMultiLine = true;
        code += "\n";
        // cancel multiline if two blank lines are entered
        if ( code.find( "\n\n\n" ) != string::npos )
            return ";";
        char * line = shellReadline( "... " , 1 );
        if ( gotInterrupted ) {
            if ( line )
                free( line );
            return "";
        }
        if ( ! line )
            return "";

        char * linePtr = line;
        while ( startsWith( linePtr, "... " ) )
            linePtr += 4;

        code += linePtr;
        free( line );
    }
    return code;
}

namespace mongo {
    namespace moe = mongo::optionenvironment;
}

moe::OptionSection options;
moe::Environment params;

std::string get_help_string(const StringData& name, const moe::OptionSection& options) {
    StringBuilder sb;
    sb << "MongoDB shell version: " << mongo::versionString << "\n";
    sb << "usage: " << name << " [options] [db address] [file names (ending in .js)]\n"
       << "db address can be:\n"
       << "  foo                   foo database on local machine\n"
       << "  192.169.0.5/foo       foo database on 192.168.0.5 machine\n"
       << "  192.169.0.5:9999/foo  foo database on 192.168.0.5 machine on port 9999\n"
       << options.helpString() << "\n"
       << "file names: a list of files to run. files have to end in .js and will exit after "
       << "unless --shell is specified";
    return sb.str();
}

bool fileExists(const std::string& file) {
    try {
#ifdef _WIN32
        boost::filesystem::path p(toWideString(file.c_str()));
#else
        boost::filesystem::path p(file);
#endif
        return boost::filesystem::exists(p);
    }
    catch ( ... ) {
        return false;
    }
}

namespace mongo {
    extern bool isShell;
}

bool execPrompt( mongo::Scope &scope, const char *promptFunction, string &prompt ) {
    string execStatement = string( "__prompt__ = " ) + promptFunction + "();";
    scope.exec( "delete __prompt__;", "", false, false, false, 0 );
    scope.exec( execStatement, "", false, false, false, 0 );
    if ( scope.type( "__prompt__" ) == String ) {
        prompt = scope.getString( "__prompt__" );
        return true;
    }
    return false;
}

/**
 * Edit a variable or input buffer text in an external editor -- EDITOR must be defined
 *
 * @param whatToEdit Name of JavaScript variable to be edited, or any text string
 */
static void edit( const string& whatToEdit ) {

    // EDITOR may be defined in the JavaScript scope or in the environment
    string editor;
    if ( shellMainScope->type( "EDITOR" ) == String ) {
        editor = shellMainScope->getString( "EDITOR" );
    }
    else {
        static const char * editorFromEnv = getenv( "EDITOR" );
        if ( editorFromEnv ) {
            editor = editorFromEnv;
        }
    }
    if ( editor.empty() ) {
        cout << "please define EDITOR as a JavaScript string or as an environment variable" << endl;
        return;
    }

    // "whatToEdit" might look like a variable/property name
    bool editingVariable = true;
    for ( const char* p = whatToEdit.c_str(); *p; ++p ) {
        if ( ! ( isalnum( *p ) || *p == '_' || *p == '.' ) ) {
            editingVariable = false;
            break;
        }
    }

    string js;
    if ( editingVariable ) {
        // If "whatToEdit" is undeclared or uninitialized, declare 
        int varType = shellMainScope->type( whatToEdit.c_str() );
        if ( varType == Undefined ) {
            shellMainScope->exec( "var " + whatToEdit , "(shell)", false, true, false );
        }

        // Convert "whatToEdit" to JavaScript (JSON) text
        if ( !shellMainScope->exec( "__jsout__ = tojson(" + whatToEdit + ")", "tojs", false, false, false ) )
            return; // Error already printed

        js = shellMainScope->getString( "__jsout__" );

        if ( strstr( js.c_str(), "[native code]" ) ) {
            cout << "can't edit native functions" << endl;
            return;
        }
    }
    else {
        js = whatToEdit;
    }

    // Pick a name to use for the temp file
    string filename;
    const int maxAttempts = 10;
    int i;
    for ( i = 0; i < maxAttempts; ++i ) {
        StringBuilder sb;
#ifdef _WIN32
        char tempFolder[MAX_PATH];
        GetTempPathA( sizeof tempFolder, tempFolder );
        sb << tempFolder << "mongo_edit" << time( 0 ) + i << ".js";
#else
        sb << "/tmp/mongo_edit" << time( 0 ) + i << ".js";
#endif
        filename = sb.str();
        if ( ! fileExists( filename ) )
            break;
    }
    if ( i == maxAttempts ) {
        cout << "couldn't create unique temp file after " << maxAttempts << " attempts" << endl;
        return;
    }

    // Create the temp file
    FILE * tempFileStream;
    tempFileStream = fopen( filename.c_str(), "wt" );
    if ( ! tempFileStream ) {
        cout << "couldn't create temp file (" << filename << "): " << errnoWithDescription() << endl;
        return;
    }

    // Write JSON into the temp file
    size_t fileSize = js.size();
    if ( fwrite( js.data(), sizeof( char ), fileSize, tempFileStream ) != fileSize ) {
        int systemErrno = errno;
        cout << "failed to write to temp file: " << errnoWithDescription( systemErrno ) << endl;
        fclose( tempFileStream );
        remove( filename.c_str() );
        return;
    }
    fclose( tempFileStream );

    // Pass file to editor
    StringBuilder sb;
    sb << editor << " " << filename;
    int ret = ::system( sb.str().c_str() );
    if ( ret ) {
        if ( ret == -1 ) {
            int systemErrno = errno;
            cout << "failed to launch $EDITOR (" << editor << "): " << errnoWithDescription( systemErrno ) << endl;
        }
        else
            cout << "editor exited with error (" << ret << "), not applying changes" << endl;
        remove( filename.c_str() );
        return;
    }

    // The editor gave return code zero, so read the file back in
    tempFileStream = fopen( filename.c_str(), "rt" );
    if ( ! tempFileStream ) {
        cout << "couldn't open temp file on return from editor: " << errnoWithDescription() << endl;
        remove( filename.c_str() );
        return;
    }
    sb.reset();
    int bytes;
    do {
        char buf[1024];
        bytes = fread( buf, sizeof( char ), sizeof buf, tempFileStream );
        if ( ferror( tempFileStream ) ) {
            cout << "failed to read temp file: " << errnoWithDescription() << endl;
            fclose( tempFileStream );
            remove( filename.c_str() );
            return;
        }
        sb.append( StringData( buf, bytes ) );
    } while ( bytes );

    // Done with temp file, close and delete it
    fclose( tempFileStream );
    remove( filename.c_str() );

    if ( editingVariable ) {
        // Try to execute assignment to copy edited value back into the variable
        const string code = whatToEdit + string( " = " ) + sb.str();
        if ( !shellMainScope->exec( code, "tojs", false, true, false ) ) {
            cout << "error executing assignment: " << code << endl;
        }
    }
    else {
        linenoisePreloadBuffer( sb.str().c_str() );
    }
}

Status addMongoShellOptions(moe::OptionSection* options) {

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status ret = options->addOption(OD("shell", "shell", moe::Switch,
                "run the shell after executing files", true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("nodb", "nodb", moe::Switch,
                "don't connect to mongod on startup - no 'db address' arg expected", true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("norc", "norc", moe::Switch,
                "will not run the \".mongorc.js\" file on start up", true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("quiet", "quiet", moe::Switch, "be less chatty", true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("port", "port", moe::String, "port to connect to" , true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("host", "host", moe::String, "server to connect to" , true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("eval", "eval", moe::String, "evaluate javascript" , true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("username", "username,u", moe::String,
                "username for authentication" , true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("password", "password,p", moe::String,
                "password for authentication" , true, moe::Value(), moe::Value(std::string(""))));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("authenticationDatabase", "authenticationDatabase", moe::String,
                "user source (defaults to dbname)" , true, moe::Value(std::string(""))));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("authenticationMechanism", "authenticationMechanism", moe::String,
                "authentication mechanism", true, moe::Value(std::string("MONGODB-CR"))));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("help", "help,h", moe::Switch, "show this usage information",
                true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("version", "version", moe::Switch, "show version information",
                true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("verbose", "verbose", moe::Switch, "increase verbosity", true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("ipv6", "ipv6", moe::Switch,
                "enable IPv6 support (disabled by default)", true));
    if (!ret.isOK()) {
        return ret;
    }
#ifdef MONGO_SSL
    ret = options->addOption(OD("ssl", "ssl", moe::Switch, "use SSL for all connections", true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("ssl.CAFile", "sslCAFile", moe::String,
                "Certificate Authority for SSL" , true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("ssl.PEMKeyFile", "sslPEMKeyFile", moe::String,
                "PEM certificate/key file for SSL" , true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("ssl.PEMKeyPassword", "sslPEMKeyPassword", moe::String,
                "password for key in PEM file for SSL" , true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("ssl.CRLFile", "sslCRLFile", moe::String,
                "Certificate Revocation List file for SSL", true));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("ssl.FIPSMode", "sslFIPSMode", moe::Switch,
                "activate FIPS 140-2 mode at startup", true));
    if (!ret.isOK()) {
        return ret;
    }
#endif

    ret = options->addOption(OD("dbaddress", "dbaddress", moe::String, "dbaddress" , false));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addOption(OD("files", "files", moe::StringVector, "files" , false));
    if (!ret.isOK()) {
        return ret;
    }
    // for testing, kill op will also be disabled automatically if the tests starts a mongo program
    ret = options->addOption(OD("nokillop", "nokillop", moe::Switch, "nokillop", false));
    if (!ret.isOK()) {
        return ret;
    }
    // for testing, will kill op without prompting
    ret = options->addOption(OD("autokillop", "autokillop", moe::Switch, "autokillop", false));
    if (!ret.isOK()) {
        return ret;
    }

    ret = options->addPositionalOption(POD("dbaddress", moe::String, 1));
    if (!ret.isOK()) {
        return ret;
    }
    ret = options->addPositionalOption(POD("files", moe::String, -1));
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status storeMongoShellOptions() {
    if ( params.count( "quiet" ) ) {
        mongo::cmdLine.quiet = true;
    }
#ifdef MONGO_SSL
    if ( params.count( "ssl" ) ) {
        mongo::cmdLine.sslOnNormalPorts = true;
    }
    if (params.count("ssl.PEMKeyFile")) {
        mongo::cmdLine.sslPEMKeyFile = params["ssl.PEMKeyFile"].as<std::string>();
    }
    if (params.count("ssl.PEMKeyPassword")) {
        mongo::cmdLine.sslPEMKeyPassword = params["ssl.PEMKeyPassword"].as<std::string>();
    }
    if (params.count("ssl.CAFile")) {
        mongo::cmdLine.sslCAFile = params["ssl.CAFile"].as<std::string>();
    }
    if (params.count("ssl.CRLFile")) {
        mongo::cmdLine.sslCRLFile = params["ssl.CRLFile"].as<std::string>();
    }
    if (params.count( "ssl.FIPSMode")) {
        mongo::cmdLine.sslFIPSMode = true;
    }
#endif
    if ( params.count( "ipv6" ) ) {
        mongo::enableIPv6();
    }
    if ( params.count( "verbose" ) ) {
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(1));
    }
    return Status::OK();
}

MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
                            ("GlobalLogManager"),
                            ("default", "completedStartupConfig"))(InitializerContext* context) {

    options = moe::OptionSection("options");
    moe::OptionsParser parser;

    Status retStatus = addMongoShellOptions(&options);
    if (!retStatus.isOK()) {
        return retStatus;
    }

    retStatus = parser.run(options, context->args(), context->env(), &params);
    if (!retStatus.isOK()) {
        StringBuilder sb;
        sb << "Error parsing options: " << retStatus.toString() << "\n";
        sb << get_help_string(context->args()[0], options);
        return Status(ErrorCodes::FailedToParse, sb.str());
    }

    // Handle storage in globals that may be needed in other MONGO_INITIALIZERS
    retStatus = storeMongoShellOptions();
    if (!retStatus.isOK()) {
        return retStatus;
    }

    return Status::OK();
}

int _main( int argc, char* argv[], char **envp ) {
    mongo::isShell = true;
    setupSignals();

    mongo::shell_utils::RecordMyLocation( argv[ 0 ] );

    string url = "test";
    string dbhost;
    string port;
    vector<string> files;

    string username;
    string password;
    string authenticationMechanism;
    string authenticationDatabase;

    std::string sslPEMKeyFile;
    std::string sslPEMKeyPassword;
    std::string sslCAFile;
    std::string sslCRLFile;

    bool runShell = false;
    bool nodb = false;
    bool norc = false;

    string script;

    mongo::runGlobalInitializersOrDie(argc, argv, envp);

    if (params.count("port")) {
        port = params["port"].as<string>();
    }

    if (params.count("host")) {
        dbhost = params["host"].as<string>();
    }

    if (params.count("eval")) {
        script = params["eval"].as<string>();
    }

    if (params.count("username")) {
        username = params["username"].as<string>();
    }

    if (params.count("password")) {
        password = params["password"].as<string>();
    }

    if (params.count("authenticationDatabase")) {
        authenticationDatabase = params["authenticationDatabase"].as<string>();
    }

    if (params.count("authenticationMechanism")) {
        authenticationMechanism = params["authenticationMechanism"].as<string>();
    }

    // hide password from ps output
    for ( int i = 0; i < (argc-1); ++i ) {
        if ( !strcmp(argv[i], "-p") || !strcmp( argv[i], "--password" ) ) {
            char* arg = argv[i + 1];
            while ( *arg ) {
                *arg++ = 'x';
            }
        }
    }

    if ( params.count( "shell" ) ) {
        runShell = true;
    }
    if ( params.count( "nodb" ) ) {
        nodb = true;
    }
    if ( params.count( "norc" ) ) {
        norc = true;
    }
    if ( params.count( "help" ) ) {
        std::cout << get_help_string(argv[0], options) << std::endl;
        return mongo::EXIT_CLEAN;
    }
    if ( params.count( "files" ) ) {
        files = params["files"].as< vector<string> >();
    }
    if ( params.count( "version" ) ) {
        cout << "MongoDB shell version: " << mongo::versionString << endl;
        return mongo::EXIT_CLEAN;
    }
    if ( params.count( "nokillop" ) ) {
        mongo::shell_utils::_nokillop = true;
    }
    if ( params.count( "autokillop" ) ) {
        autoKillOp = true;
    }

    /* This is a bit confusing, here are the rules:
     *
     * if nodb is set then all positional parameters are files
     * otherwise the first positional parameter might be a dbaddress, but
     * only if one of these conditions is met:
     *   - it contains no '.' after the last appearance of '\' or '/'
     *   - it doesn't end in '.js' and it doesn't specify a path to an existing file */
    if ( params.count( "dbaddress" ) ) {
        string dbaddress = params["dbaddress"].as<string>();
        if (nodb) {
            files.insert( files.begin(), dbaddress );
        }
        else {
            string basename = dbaddress.substr( dbaddress.find_last_of( "/\\" ) + 1 );
            if (basename.find_first_of( '.' ) == string::npos ||
                    ( basename.find( ".js", basename.size() - 3 ) == string::npos && !fileExists( dbaddress ) ) ) {
                url = dbaddress;
            }
            else {
                files.insert( files.begin(), dbaddress );
            }
        }
    }

    if ( url == "*" ) {
        std::cerr << "ERROR: " << "\"*\" is an invalid db address" << std::endl;
        std::cerr << get_help_string(argv[0], options) << std::endl;
        return mongo::EXIT_BADOPTIONS;
    }

    if ( ! mongo::cmdLine.quiet )
        cout << "MongoDB shell version: " << mongo::versionString << endl;

    mongo::StartupTest::runTests();

    logger::globalLogManager()->getNamedDomain("javascriptOutput")->attachAppender(
            logger::MessageLogDomain::AppenderAutoPtr(
                    new logger::ConsoleAppender<logger::MessageEventEphemeral>(
                            new logger::MessageEventUnadornedEncoder)));

    if ( !nodb ) { // connect to db
        //if ( ! mongo::cmdLine.quiet ) cout << "url: " << url << endl;

        stringstream ss;
        if ( mongo::cmdLine.quiet )
            ss << "__quiet = true;";
        ss << "db = connect( \"" << fixHost( url , dbhost , port ) << "\")";

        mongo::shell_utils::_dbConnect = ss.str();

        if ( params.count( "password" ) && password.empty() )
            password = mongo::askPassword();
    }

    // Construct the authentication-related code to execute on shell startup.
    //
    // This constructs and immediately executes an anonymous function, to avoid
    // the shell's default behavior of printing statement results to the console.
    //
    // It constructs a statement of the following form:
    //
    // (function() {
    //    // Set default authentication mechanism and, maybe, authenticate.
    //  }())
    stringstream authStringStream;
    authStringStream << "(function() { " << endl;
    if ( !authenticationMechanism.empty() ) {
        authStringStream << "DB.prototype._defaultAuthenticationMechanism = \"" <<
            authenticationMechanism << "\";" << endl;
    }

    if (!nodb && username.size()) {
        authStringStream << "var username = \"" << username << "\";" << endl;
        authStringStream << "var password = \"" << password << "\";" << endl;
        if (authenticationDatabase.empty()) {
            authStringStream << "var authDb = db;" << endl;
        }
        else {
            authStringStream << "var authDb = db.getSiblingDB(\"" << authenticationDatabase <<
                "\");" << endl;
        }
        authStringStream << "authDb._authOrThrow({ " <<
            saslCommandUserFieldName << ": username, " <<
            saslCommandPasswordFieldName << ": password });" << endl;
    }
    authStringStream << "}())";
    mongo::shell_utils::_dbAuth = authStringStream.str();


    mongo::ScriptEngine::setConnectCallback( mongo::shell_utils::onConnect );
    mongo::ScriptEngine::setup();
    mongo::globalScriptEngine->setScopeInitCallback( mongo::shell_utils::initScope );
    auto_ptr< mongo::Scope > scope( mongo::globalScriptEngine->newScope() );
    shellMainScope = scope.get();

    if( runShell )
        cout << "type \"help\" for help" << endl;
   
    // Load and execute /etc/mongorc.js before starting shell
    std::string rcGlobalLocation;
#ifndef _WIN32
    rcGlobalLocation = "/etc/mongorc.js" ;
#else
    wchar_t programDataPath[MAX_PATH];
    if ( S_OK == SHGetFolderPathW(NULL,
                                CSIDL_COMMON_APPDATA,
                                NULL,
                                0,
                                programDataPath) ) {
        rcGlobalLocation = str::stream() << toUtf8String(programDataPath)
                                         << "\\MongoDB\\mongorc.js";
    }
#endif   
    if ( !rcGlobalLocation.empty() && fileExists(rcGlobalLocation) ) {
        if ( ! scope->execFile( rcGlobalLocation , false , true ) ) {
            cout << "The \"" << rcGlobalLocation << "\" file could not be executed" << endl;
        }
    }

    if ( !script.empty() ) {
        mongo::shell_utils::MongoProgramScope s;
        if ( ! scope->exec( script , "(shell eval)" , true , true , false ) )
            return -4;
    }

    for (size_t i = 0; i < files.size(); ++i) {
        mongo::shell_utils::MongoProgramScope s;

        if ( files.size() > 1 )
            cout << "loading file: " << files[i] << endl;

        if ( ! scope->execFile( files[i] , false , true ) ) {
            cout << "failed to load: " << files[i] << endl;
            return -3;
        }
    }

    if ( files.size() == 0 && script.empty() )
        runShell = true;

    if ( runShell ) {

        mongo::shell_utils::MongoProgramScope s;
        bool hasMongoRC = norc; // If they specify norc, assume it's not their first time
        string rcLocation;
        if ( !norc ) {
#ifndef _WIN32
            if ( getenv( "HOME" ) != NULL )
                rcLocation = str::stream() << getenv( "HOME" ) << "/.mongorc.js" ;
#else
            if ( getenv( "HOMEDRIVE" ) != NULL && getenv( "HOMEPATH" ) != NULL )
                rcLocation = str::stream() << toUtf8String(_wgetenv(L"HOMEDRIVE"))
                                           << toUtf8String(_wgetenv(L"HOMEPATH"))
                                           << "\\.mongorc.js";
#endif
            if ( !rcLocation.empty() && fileExists(rcLocation) ) {
                hasMongoRC = true;
                if ( ! scope->execFile( rcLocation , false , true ) ) {
                    cout << "The \".mongorc.js\" file located in your home folder could not be executed" << endl;
                    return -5;
                }
            }
        }

        if ( !hasMongoRC && isatty(fileno(stdin)) ) {
            cout << "Welcome to the MongoDB shell.\n"
                    "For interactive help, type \"help\".\n"
                    "For more comprehensive documentation, see\n\thttp://docs.mongodb.org/\n"
                    "Questions? Try the support group\n\thttp://groups.google.com/group/mongodb-user" << endl;
            File f;
            f.open(rcLocation.c_str(), false);  // Create empty .mongorc.js file
        }

        if ( !nodb && !mongo::cmdLine.quiet && isatty(fileno(stdin)) ) {
            scope->exec( "shellHelper( 'show', 'startupWarnings' )", "(shellwarnings", false, true, false );
        }

        shellHistoryInit();

        string prompt;
        int promptType;

        //v8::Handle<v8::Object> shellHelper = baseContext_->Global()->Get( v8::String::New( "shellHelper" ) )->ToObject();

        while ( 1 ) {
            inMultiLine = false;
            gotInterrupted = false;
//            shellMainScope->localConnect;
            //DBClientWithCommands *c = getConnection( JSContext *cx, JSObject *obj );

            promptType = scope->type( "prompt" );
            if ( promptType == String ) {
                prompt = scope->getString( "prompt" );
            }
            else if ( ( promptType == Code ) &&
                     execPrompt( *scope, "prompt", prompt ) ) {
            }
            else if ( execPrompt( *scope, "defaultPrompt", prompt ) ) {
            }
            else {
                prompt = "> ";
            }

            char * line = shellReadline( prompt.c_str() );

            char * linePtr = line;  // can't clobber 'line', we need to free() it later
            if ( linePtr ) {
                while ( linePtr[0] == ' ' )
                    ++linePtr;
                int lineLen = strlen( linePtr );
                while ( lineLen > 0 && linePtr[lineLen - 1] == ' ' )
                    linePtr[--lineLen] = 0;
            }

            if ( ! linePtr || ( strlen( linePtr ) == 4 && strstr( linePtr , "exit" ) ) ) {
                if ( ! mongo::cmdLine.quiet )
                    cout << "bye" << endl;
                if ( line )
                    free( line );
                break;
            }

            string code = linePtr;
            if ( code == "exit" || code == "exit;" ) {
                free( line );
                break;
            }
            if ( code == "cls" ) {
                free( line );
                linenoiseClearScreen();
                continue;
            }

            if ( code.size() == 0 ) {
                free( line );
                continue;
            }

            if ( startsWith( linePtr, "edit " ) ) {
                shellHistoryAdd( linePtr );

                const char* s = linePtr + 5; // skip "edit "
                while( *s && isspace( *s ) )
                    s++;

                edit( s );
                free( line );
                continue;
            }

            gotInterrupted = false;
            code = finishCode( code );
            if ( gotInterrupted ) {
                cout << endl;
                free( line );
                continue;
            }

            if ( code.size() == 0 ) {
                free( line );
                break;
            }

            bool wascmd = false;
            {
                string cmd = linePtr;
                if ( cmd.find( " " ) > 0 )
                    cmd = cmd.substr( 0 , cmd.find( " " ) );

                if ( cmd.find( "\"" ) == string::npos ) {
                    try {
                        scope->exec( (string)"__iscmd__ = shellHelper[\"" + cmd + "\"];" , "(shellhelp1)" , false , true , true );
                        if ( scope->getBoolean( "__iscmd__" )  ) {
                            scope->exec( (string)"shellHelper( \"" + cmd + "\" , \"" + code.substr( cmd.size() ) + "\");" , "(shellhelp2)" , false , true , false );
                            wascmd = true;
                        }
                    }
                    catch ( std::exception& e ) {
                        cout << "error2:" << e.what() << endl;
                        wascmd = true;
                    }
                }
            }

            if ( ! wascmd ) {
                try {
                    if ( scope->exec( code.c_str() , "(shell)" , false , true , false ) )
                        scope->exec( "shellPrintHelper( __lastres__ );" , "(shell2)" , true , true , false );
                }
                catch ( std::exception& e ) {
                    cout << "error:" << e.what() << endl;
                }
            }

            shellHistoryAdd( code.c_str() );
            free( line );
        }

        shellHistoryDone();
    }

    mongo::dbexitCalled = true;
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    static mongo::StaticObserver staticObserver;
    int returnCode;
    try {
        WindowsCommandLine wcl(argc, argvW, envpW);
        returnCode = _main(argc, wcl.argv(), wcl.envp());
    }
    catch ( mongo::DBException& e ) {
        cerr << "exception: " << e.what() << endl;
        returnCode = 1;
    }
    ::_exit(returnCode);
}
#else // #ifdef _WIN32
int main( int argc, char* argv[], char **envp ) {
    static mongo::StaticObserver staticObserver;
    int returnCode;
    try {
        returnCode = _main( argc , argv, envp );
    }
    catch ( mongo::DBException& e ) {
        cerr << "exception: " << e.what() << endl;
        returnCode = 1;
    }
    _exit(returnCode);
}
#endif // #ifdef _WIN32
