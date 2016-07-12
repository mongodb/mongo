// dbshell.cpp
/*
 *    Copyright 2010 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <boost/filesystem/operations.hpp>
#include <fstream>
#include <iostream>
#include <pcrecpp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/client.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/server_options.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/linenoise.h"
#include "mongo/shell/shell_options.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/util/exit.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/password.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/static_observer.h"
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
static volatile bool atPrompt = false;  // can eval before getting to prompt

namespace mongo {

Scope* shellMainScope;
}

void generateCompletions(const string& prefix, vector<string>& all) {
    if (prefix.find('"') != string::npos)
        return;

    try {
        BSONObj args = BSON("0" << prefix);
        shellMainScope->invokeSafe(
            "function callShellAutocomplete(x) {shellAutocomplete(x)}", &args, NULL);
        BSONObjBuilder b;
        shellMainScope->append(b, "", "__autocomplete__");
        BSONObj res = b.obj();
        BSONObj arr = res.firstElement().Obj();

        BSONObjIterator i(arr);
        while (i.more()) {
            BSONElement e = i.next();
            all.push_back(e.String());
        }
    } catch (...) {
    }
}

void completionHook(const char* text, linenoiseCompletions* lc) {
    vector<string> all;
    generateCompletions(text, all);

    for (unsigned i = 0; i < all.size(); ++i)
        linenoiseAddCompletion(lc, (char*)all[i].c_str());
}

void shellHistoryInit() {
    stringstream ss;
    const char* h = shell_utils::getUserDir();
    if (h)
        ss << h << "/";
    ss << ".dbshell";
    historyFile = ss.str();

    linenoiseHistoryLoad(historyFile.c_str());
    linenoiseSetCompletionCallback(completionHook);
}

void shellHistoryDone() {
    linenoiseHistorySave(historyFile.c_str());
    linenoiseHistoryFree();
}
void shellHistoryAdd(const char* line) {
    if (line[0] == '\0')
        return;

    // dont record duplicate lines
    static string lastLine;
    if (lastLine == line)
        return;
    lastLine = line;

    // We don't want any .auth() or .createUser() shell helpers added, but we want to
    // be able to add things like `.author`, so be smart about how this is
    // detected by using regular expresions. This is so we can avoid storing passwords
    // in the history file in plaintext.
    static pcrecpp::RE hiddenHelpers(
        "\\.\\s*(auth|createUser|updateUser|changeUserPassword)\\s*\\(");
    // Also don't want the raw user management commands to show in the shell when run directly
    // via runCommand.
    static pcrecpp::RE hiddenCommands(
        "(run|admin)Command\\s*\\(\\s*{\\s*(createUser|updateUser)\\s*:");
    if (!hiddenHelpers.PartialMatch(line) && !hiddenCommands.PartialMatch(line)) {
        linenoiseHistoryAdd(line);
    }
}

void killOps() {
    if (mongo::shell_utils::_nokillop)
        return;

    if (atPrompt)
        return;

    sleepmillis(10);  // give current op a chance to finish

    mongo::shell_utils::connectionRegistry.killOperationsOnAllConnections(
        !shellGlobalParams.autoKillOp);
}

// Stubs for signal_handlers.cpp
namespace mongo {
void logProcessDetailsForLogRotate() {}
}

void quitNicely(int sig) {
    shutdown(EXIT_CLEAN);
}

// the returned string is allocated with strdup() or malloc() and must be freed by calling free()
char* shellReadline(const char* prompt, int handlesigint = 0) {
    atPrompt = true;

    char* ret = linenoise(prompt);
    if (!ret) {
        gotInterrupted = true;  // got ^C, break out of multiline
    }

    atPrompt = false;
    return ret;
}

void setupSignals() {
    signal(SIGINT, quitNicely);
}

string fixHost(const std::string& url, const std::string& host, const std::string& port) {
    if (host.size() == 0 && port.size() == 0) {
        if (url.find("/") == string::npos) {
            // check for ips
            if (url.find(".") != string::npos)
                return url + "/test";

            if (url.rfind(":") != string::npos && isdigit(url[url.rfind(":") + 1]))
                return url + "/test";
        }
        return url;
    }

    if (url.find("/") != string::npos) {
        cerr << "url can't have host or port if you specify them individually" << endl;
        quickExit(-1);
    }

    string newurl((host.size() == 0) ? "127.0.0.1" : host);
    if (port.size() > 0)
        newurl += ":" + port;
    else if (host.find(':') == string::npos) {
        // need to add port with IPv6 addresses
        newurl += ":27017";
    }

    newurl += "/" + url;

    return newurl;
}

static string OpSymbols = "~!%^&*-+=|:,<>/?.";

bool isOpSymbol(char c) {
    for (size_t i = 0; i < OpSymbols.size(); i++)
        if (OpSymbols[i] == c)
            return true;
    return false;
}

bool isUseCmd(const std::string& code) {
    string cmd = code;
    if (cmd.find(" ") > 0)
        cmd = cmd.substr(0, cmd.find(" "));
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
        if (start == pos || code[pos - 1] != '\\' ||  // previous char was backslash
            start == pos - 1 ||
            code[pos - 2] == '\\'  // char before backslash was not another
            ) {
            break;  // The quote we found was not preceded by an unescaped backslash; it is real
        }
        ++pos;  // The quote we found was escaped with backslash, so it doesn't count
    }
    return pos;
}

bool isBalanced(const std::string& code) {
    if (isUseCmd(code))
        return true;  // don't balance "use <dbname>" in case dbname contains special chars
    int curlyBrackets = 0;
    int squareBrackets = 0;
    int parens = 0;
    bool danglingOp = false;

    for (size_t i = 0; i < code.size(); i++) {
        switch (code[i]) {
            case '/':
                if (i + 1 < code.size() && code[i + 1] == '/') {
                    while (i < code.size() && code[i] != '\n')
                        i++;
                }
                continue;
            case '{':
                curlyBrackets++;
                break;
            case '}':
                if (curlyBrackets <= 0)
                    return true;
                curlyBrackets--;
                break;
            case '[':
                squareBrackets++;
                break;
            case ']':
                if (squareBrackets <= 0)
                    return true;
                squareBrackets--;
                break;
            case '(':
                parens++;
                break;
            case ')':
                if (parens <= 0)
                    return true;
                parens--;
                break;
            case '"':
            case '\'':
                i = skipOverString(code, i + 1, code[i]);
                if (i >= code.size()) {
                    return true;  // Do not let unterminated strings enter multi-line mode
                }
                break;
            case '\\':
                if (i + 1 < code.size() && code[i + 1] == '/')
                    i++;
                break;
            case '+':
            case '-':
                if (i + 1 < code.size() && code[i + 1] == code[i]) {
                    i++;
                    continue;  // postfix op (++/--) can't be a dangling op
                }
                break;
        }
        if (i >= code.size()) {
            danglingOp = false;
            break;
        }
        if (isOpSymbol(code[i]))
            danglingOp = true;
        else if (!std::isspace(static_cast<unsigned char>(code[i])))
            danglingOp = false;
    }

    return curlyBrackets == 0 && squareBrackets == 0 && parens == 0 && !danglingOp;
}

struct BalancedTest : public mongo::StartupTest {
public:
    void run() {
        verify(isBalanced("x = 5"));
        verify(isBalanced("function(){}"));
        verify(isBalanced("function(){\n}"));
        verify(!isBalanced("function(){"));
        verify(isBalanced("x = \"{\";"));
        verify(isBalanced("// {"));
        verify(!isBalanced("// \n {"));
        verify(!isBalanced("\"//\" {"));
        verify(isBalanced("{x:/x\\//}"));
        verify(!isBalanced("{ \\/// }"));
        verify(isBalanced("x = 5 + y "));
        verify(!isBalanced("x = "));
        verify(!isBalanced("x = // hello"));
        verify(!isBalanced("x = 5 +"));
        verify(isBalanced(" x ++"));
        verify(isBalanced("-- x"));
        verify(!isBalanced("a."));
        verify(!isBalanced("a. "));
        verify(isBalanced("a.b"));

        // SERVER-5809 and related cases --
        verify(isBalanced("a = {s:\"\\\"\"}"));            // a = {s:"\""}
        verify(isBalanced("db.test.save({s:\"\\\"\"})"));  // db.test.save({s:"\""})
        verify(isBalanced("printjson(\" \\\" \")"));       // printjson(" \" ") -- SERVER-8554
        verify(isBalanced("var a = \"\\\\\";"));           // var a = "\\";
        verify(isBalanced("var a = (\"\\\\\") //\""));     // var a = ("\\") //"
        verify(isBalanced("var a = (\"\\\\\") //\\\""));   // var a = ("\\") //\"
        verify(isBalanced("var a = (\"\\\\\") //"));       // var a = ("\\") //
        verify(isBalanced("var a = (\"\\\\\")"));          // var a = ("\\")
        verify(isBalanced("var a = (\"\\\\\\\"\")"));      // var a = ("\\\"")
        verify(!isBalanced("var a = (\"\\\\\" //\""));     // var a = ("\\" //"
        verify(!isBalanced("var a = (\"\\\\\" //"));       // var a = ("\\" //
        verify(!isBalanced("var a = (\"\\\\\""));          // var a = ("\\"
    }
} balanced_test;

string finishCode(string code) {
    while (!isBalanced(code)) {
        inMultiLine = true;
        code += "\n";
        // cancel multiline if two blank lines are entered
        if (code.find("\n\n\n") != string::npos)
            return ";";
        char* line = shellReadline("... ", 1);
        if (gotInterrupted) {
            if (line)
                free(line);
            return "";
        }
        if (!line)
            return "";

        char* linePtr = line;
        while (str::startsWith(linePtr, "... "))
            linePtr += 4;

        code += linePtr;
        free(line);
    }
    return code;
}

bool execPrompt(mongo::Scope& scope, const char* promptFunction, string& prompt) {
    string execStatement = string("__prompt__ = ") + promptFunction + "();";
    scope.exec("delete __prompt__;", "", false, false, false, 0);
    scope.exec(execStatement, "", false, false, false, 0);
    if (scope.type("__prompt__") == String) {
        prompt = scope.getString("__prompt__");
        return true;
    }
    return false;
}

/**
 * Edit a variable or input buffer text in an external editor -- EDITOR must be defined
 *
 * @param whatToEdit Name of JavaScript variable to be edited, or any text string
 */
static void edit(const string& whatToEdit) {
    // EDITOR may be defined in the JavaScript scope or in the environment
    string editor;
    if (shellMainScope->type("EDITOR") == String) {
        editor = shellMainScope->getString("EDITOR");
    } else {
        static const char* editorFromEnv = getenv("EDITOR");
        if (editorFromEnv) {
            editor = editorFromEnv;
        }
    }
    if (editor.empty()) {
        cout << "please define EDITOR as a JavaScript string or as an environment variable" << endl;
        return;
    }

    // "whatToEdit" might look like a variable/property name
    bool editingVariable = true;
    for (const char* p = whatToEdit.c_str(); *p; ++p) {
        if (!(isalnum(*p) || *p == '_' || *p == '.')) {
            editingVariable = false;
            break;
        }
    }

    string js;
    if (editingVariable) {
        // If "whatToEdit" is undeclared or uninitialized, declare
        int varType = shellMainScope->type(whatToEdit.c_str());
        if (varType == Undefined) {
            shellMainScope->exec("var " + whatToEdit, "(shell)", false, true, false);
        }

        // Convert "whatToEdit" to JavaScript (JSON) text
        if (!shellMainScope->exec(
                "__jsout__ = tojson(" + whatToEdit + ")", "tojs", false, false, false))
            return;  // Error already printed

        js = shellMainScope->getString("__jsout__");

        if (strstr(js.c_str(), "[native code]")) {
            cout << "can't edit native functions" << endl;
            return;
        }
    } else {
        js = whatToEdit;
    }

    // Pick a name to use for the temp file
    string filename;
    const int maxAttempts = 10;
    int i;
    for (i = 0; i < maxAttempts; ++i) {
        StringBuilder sb;
#ifdef _WIN32
        char tempFolder[MAX_PATH];
        GetTempPathA(sizeof tempFolder, tempFolder);
        sb << tempFolder << "mongo_edit" << time(0) + i << ".js";
#else
        sb << "/tmp/mongo_edit" << time(0) + i << ".js";
#endif
        filename = sb.str();
        if (!::mongo::shell_utils::fileExists(filename))
            break;
    }
    if (i == maxAttempts) {
        cout << "couldn't create unique temp file after " << maxAttempts << " attempts" << endl;
        return;
    }

    // Create the temp file
    FILE* tempFileStream;
    tempFileStream = fopen(filename.c_str(), "wt");
    if (!tempFileStream) {
        cout << "couldn't create temp file (" << filename << "): " << errnoWithDescription()
             << endl;
        return;
    }

    // Write JSON into the temp file
    size_t fileSize = js.size();
    if (fwrite(js.data(), sizeof(char), fileSize, tempFileStream) != fileSize) {
        int systemErrno = errno;
        cout << "failed to write to temp file: " << errnoWithDescription(systemErrno) << endl;
        fclose(tempFileStream);
        remove(filename.c_str());
        return;
    }
    fclose(tempFileStream);

    // Pass file to editor
    StringBuilder sb;
    sb << editor << " " << filename;
    int ret = ::system(sb.str().c_str());
    if (ret) {
        if (ret == -1) {
            int systemErrno = errno;
            cout << "failed to launch $EDITOR (" << editor
                 << "): " << errnoWithDescription(systemErrno) << endl;
        } else
            cout << "editor exited with error (" << ret << "), not applying changes" << endl;
        remove(filename.c_str());
        return;
    }

    // The editor gave return code zero, so read the file back in
    tempFileStream = fopen(filename.c_str(), "rt");
    if (!tempFileStream) {
        cout << "couldn't open temp file on return from editor: " << errnoWithDescription() << endl;
        remove(filename.c_str());
        return;
    }
    sb.reset();
    int bytes;
    do {
        char buf[1024];
        bytes = fread(buf, sizeof(char), sizeof buf, tempFileStream);
        if (ferror(tempFileStream)) {
            cout << "failed to read temp file: " << errnoWithDescription() << endl;
            fclose(tempFileStream);
            remove(filename.c_str());
            return;
        }
        sb.append(StringData(buf, bytes));
    } while (bytes);

    // Done with temp file, close and delete it
    fclose(tempFileStream);
    remove(filename.c_str());

    if (editingVariable) {
        // Try to execute assignment to copy edited value back into the variable
        const string code = whatToEdit + string(" = ") + sb.str();
        if (!shellMainScope->exec(code, "tojs", false, true, false)) {
            cout << "error executing assignment: " << code << endl;
        }
    } else {
        linenoisePreloadBuffer(sb.str().c_str());
    }
}

int _main(int argc, char* argv[], char** envp) {
    registerShutdownTask([] {
        // NOTE: This function may be called at any time. It must not
        // depend on the prior execution of mongo initializers or the
        // existence of threads.
        ::killOps();
        ::shellHistoryDone();
    });

    setupSignalHandlers();
    setupSignals();

    mongo::shell_utils::RecordMyLocation(argv[0]);

    shellGlobalParams.url = "test";

    mongo::runGlobalInitializersOrDie(argc, argv, envp);

    // hide password from ps output
    for (int i = 0; i < (argc - 1); ++i) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) {
            char* arg = argv[i + 1];
            while (*arg) {
                *arg++ = 'x';
            }
        }
    }

    if (!mongo::serverGlobalParams.quiet)
        cout << "MongoDB shell version: " << mongo::versionString << endl;

    mongo::StartupTest::runTests();

    logger::globalLogManager()
        ->getNamedDomain("javascriptOutput")
        ->attachAppender(logger::MessageLogDomain::AppenderAutoPtr(
            new logger::ConsoleAppender<logger::MessageEventEphemeral>(
                new logger::MessageEventUnadornedEncoder)));

    if (!shellGlobalParams.nodb) {  // connect to db
        stringstream ss;
        if (mongo::serverGlobalParams.quiet)
            ss << "__quiet = true;";
        ss << "db = connect( \""
           << fixHost(shellGlobalParams.url, shellGlobalParams.dbhost, shellGlobalParams.port)
           << "\")";

        mongo::shell_utils::_dbConnect = ss.str();

        if (shellGlobalParams.usingPassword && shellGlobalParams.password.empty()) {
            shellGlobalParams.password = mongo::askPassword();
        }
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
    if (!shellGlobalParams.authenticationMechanism.empty()) {
        authStringStream << "DB.prototype._defaultAuthenticationMechanism = \""
                         << escape(shellGlobalParams.authenticationMechanism) << "\";" << endl;
    }

    if (!shellGlobalParams.gssapiServiceName.empty()) {
        authStringStream << "DB.prototype._defaultGssapiServiceName = \""
                         << escape(shellGlobalParams.gssapiServiceName) << "\";" << endl;
    }

    if (!shellGlobalParams.nodb && shellGlobalParams.username.size()) {
        authStringStream << "var username = \"" << escape(shellGlobalParams.username) << "\";"
                         << endl;
        if (shellGlobalParams.usingPassword) {
            authStringStream << "var password = \"" << escape(shellGlobalParams.password) << "\";"
                             << endl;
        }
        if (shellGlobalParams.authenticationDatabase.empty()) {
            authStringStream << "var authDb = db;" << endl;
        } else {
            authStringStream << "var authDb = db.getSiblingDB(\""
                             << escape(shellGlobalParams.authenticationDatabase) << "\");" << endl;
        }
        authStringStream << "authDb._authOrThrow({ " << saslCommandUserFieldName << ": username ";
        if (shellGlobalParams.usingPassword) {
            authStringStream << ", " << saslCommandPasswordFieldName << ": password ";
        }

        if (!shellGlobalParams.gssapiHostName.empty()) {
            authStringStream << ", " << saslCommandServiceHostnameFieldName << ": \""
                             << escape(shellGlobalParams.gssapiHostName) << '"' << endl;
        }
        authStringStream << "});" << endl;
    }
    authStringStream << "}())";
    mongo::shell_utils::_dbAuth = authStringStream.str();

    mongo::ScriptEngine::setConnectCallback(mongo::shell_utils::onConnect);
    mongo::ScriptEngine::setup();
    mongo::globalScriptEngine->setScopeInitCallback(mongo::shell_utils::initScope);
    mongo::globalScriptEngine->enableJIT(!shellGlobalParams.nojit);
    mongo::globalScriptEngine->enableJavaScriptProtection(shellGlobalParams.javascriptProtection);

    auto poolGuard = MakeGuard([] { ScriptEngine::dropScopeCache(); });

    unique_ptr<mongo::Scope> scope(mongo::globalScriptEngine->newScope());
    shellMainScope = scope.get();

    if (shellGlobalParams.runShell)
        cout << "type \"help\" for help" << endl;

    // Load and execute /etc/mongorc.js before starting shell
    std::string rcGlobalLocation;
#ifndef _WIN32
    rcGlobalLocation = "/etc/mongorc.js";
#else
    wchar_t programDataPath[MAX_PATH];
    if (S_OK == SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, programDataPath)) {
        rcGlobalLocation = str::stream() << toUtf8String(programDataPath)
                                         << "\\MongoDB\\mongorc.js";
    }
#endif
    if (!rcGlobalLocation.empty() && ::mongo::shell_utils::fileExists(rcGlobalLocation)) {
        if (!scope->execFile(rcGlobalLocation, false, true)) {
            cout << "The \"" << rcGlobalLocation << "\" file could not be executed" << endl;
        }
    }

    if (!shellGlobalParams.script.empty()) {
        mongo::shell_utils::MongoProgramScope s;
        if (!scope->exec(shellGlobalParams.script, "(shell eval)", false, true, false))
            return -4;
        scope->exec("shellPrintHelper( __lastres__ );", "(shell2 eval)", true, true, false);
    }

    for (size_t i = 0; i < shellGlobalParams.files.size(); ++i) {
        mongo::shell_utils::MongoProgramScope s;

        if (shellGlobalParams.files.size() > 1)
            cout << "loading file: " << shellGlobalParams.files[i] << endl;

        if (!scope->execFile(shellGlobalParams.files[i], false, true)) {
            cout << "failed to load: " << shellGlobalParams.files[i] << endl;
            return -3;
        }
    }

    if (shellGlobalParams.files.size() == 0 && shellGlobalParams.script.empty())
        shellGlobalParams.runShell = true;

    bool lastLineSuccessful = true;
    if (shellGlobalParams.runShell) {
        mongo::shell_utils::MongoProgramScope s;
        // If they specify norc, assume it's not their first time
        bool hasMongoRC = shellGlobalParams.norc;
        string rcLocation;
        if (!shellGlobalParams.norc) {
#ifndef _WIN32
            if (getenv("HOME") != NULL)
                rcLocation = str::stream() << getenv("HOME") << "/.mongorc.js";
#else
            if (getenv("HOMEDRIVE") != NULL && getenv("HOMEPATH") != NULL)
                rcLocation = str::stream() << toUtf8String(_wgetenv(L"HOMEDRIVE"))
                                           << toUtf8String(_wgetenv(L"HOMEPATH"))
                                           << "\\.mongorc.js";
#endif
            if (!rcLocation.empty() && ::mongo::shell_utils::fileExists(rcLocation)) {
                hasMongoRC = true;
                if (!scope->execFile(rcLocation, false, true)) {
                    cout << "The \".mongorc.js\" file located in your home folder could not be "
                            "executed"
                         << endl;
                    return -5;
                }
            }
        }

        if (!hasMongoRC && isatty(fileno(stdin))) {
            cout
                << "Welcome to the MongoDB shell.\n"
                   "For interactive help, type \"help\".\n"
                   "For more comprehensive documentation, see\n\thttp://docs.mongodb.org/\n"
                   "Questions? Try the support group\n\thttp://groups.google.com/group/mongodb-user"
                << endl;
            File f;
            f.open(rcLocation.c_str(), false);  // Create empty .mongorc.js file
        }

        if (!shellGlobalParams.nodb && !mongo::serverGlobalParams.quiet && isatty(fileno(stdin))) {
            scope->exec(
                "shellHelper( 'show', 'startupWarnings' )", "(shellwarnings)", false, true, false);

            scope->exec("shellHelper( 'show', 'automationNotices' )",
                        "(automationnotices)",
                        false,
                        true,
                        false);
        }

        shellHistoryInit();

        string prompt;
        int promptType;

        while (1) {
            inMultiLine = false;
            gotInterrupted = false;

            promptType = scope->type("prompt");
            if (promptType == String) {
                prompt = scope->getString("prompt");
            } else if ((promptType == Code) && execPrompt(*scope, "prompt", prompt)) {
            } else if (execPrompt(*scope, "defaultPrompt", prompt)) {
            } else {
                prompt = "> ";
            }

            char* line = shellReadline(prompt.c_str());

            char* linePtr = line;  // can't clobber 'line', we need to free() it later
            if (linePtr) {
                while (linePtr[0] == ' ')
                    ++linePtr;
                int lineLen = strlen(linePtr);
                while (lineLen > 0 && linePtr[lineLen - 1] == ' ')
                    linePtr[--lineLen] = 0;
            }

            if (!linePtr || (strlen(linePtr) == 4 && strstr(linePtr, "exit"))) {
                if (!mongo::serverGlobalParams.quiet)
                    cout << "bye" << endl;
                if (line)
                    free(line);
                break;
            }

            string code = linePtr;
            if (code == "exit" || code == "exit;") {
                free(line);
                break;
            }
            if (code == "cls") {
                free(line);
                linenoiseClearScreen();
                continue;
            }

            if (code.size() == 0) {
                free(line);
                continue;
            }

            if (str::startsWith(linePtr, "edit ")) {
                shellHistoryAdd(linePtr);

                const char* s = linePtr + 5;  // skip "edit "
                while (*s && isspace(*s))
                    s++;

                edit(s);
                free(line);
                continue;
            }

            gotInterrupted = false;
            code = finishCode(code);
            if (gotInterrupted) {
                cout << endl;
                free(line);
                continue;
            }

            if (code.size() == 0) {
                free(line);
                break;
            }

            bool wascmd = false;
            {
                string cmd = linePtr;
                string::size_type firstSpace;
                if ((firstSpace = cmd.find(" ")) != string::npos)
                    cmd = cmd.substr(0, firstSpace);

                if (cmd.find("\"") == string::npos) {
                    try {
                        lastLineSuccessful =
                            scope->exec((string) "__iscmd__ = shellHelper[\"" + cmd + "\"];",
                                        "(shellhelp1)",
                                        false,
                                        true,
                                        true);
                        if (scope->getBoolean("__iscmd__")) {
                            lastLineSuccessful =
                                scope->exec((string) "shellHelper( \"" + cmd + "\" , \"" +
                                                code.substr(cmd.size()) + "\");",
                                            "(shellhelp2)",
                                            false,
                                            true,
                                            false);
                            wascmd = true;
                        }
                    } catch (std::exception& e) {
                        cout << "error2:" << e.what() << endl;
                        wascmd = true;
                        lastLineSuccessful = false;
                    }
                }
            }

            if (!wascmd) {
                try {
                    lastLineSuccessful = scope->exec(code.c_str(), "(shell)", false, true, false);
                    if (lastLineSuccessful) {
                        scope->exec(
                            "shellPrintHelper( __lastres__ );", "(shell2)", true, true, false);
                    }
                } catch (std::exception& e) {
                    cout << "error:" << e.what() << endl;
                    lastLineSuccessful = false;
                }
            }

            shellHistoryAdd(code.c_str());
            free(line);
        }

        shellHistoryDone();
    }

    return (lastLineSuccessful ? 0 : 1);
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    static mongo::StaticObserver staticObserver;
    int returnCode;
    try {
        WindowsCommandLine wcl(argc, argvW, envpW);
        returnCode = _main(argc, wcl.argv(), wcl.envp());
    } catch (mongo::DBException& e) {
        cerr << "exception: " << e.what() << endl;
        returnCode = 1;
    }
    quickExit(returnCode);
}
#else   // #ifdef _WIN32
int main(int argc, char* argv[], char** envp) {
    static mongo::StaticObserver staticObserver;
    int returnCode;
    try {
        returnCode = _main(argc, argv, envp);
    } catch (mongo::DBException& e) {
        cerr << "exception: " << e.what() << endl;
        returnCode = 1;
    }
    quickExit(returnCode);
}
#endif  // #ifdef _WIN32
