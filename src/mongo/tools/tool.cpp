/*
 *    Copyright (C) 2010 10gen Inc.
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

// Tool.cpp

#include "mongo/tools/tool.h"

#include <boost/filesystem/operations.hpp>
#include <fstream>
#include <iostream>
#include <limits>

#include "mongo/base/initializer.h"
#include "mongo/base/init.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/global_environment_d.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/repl_coordinator_mock.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/posix_fadvise.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/password.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

using namespace std;
using namespace mongo;

namespace mongo {

    Tool::Tool() :
        _autoreconnect(false), _conn(0), _slaveConn(0) { }

    Tool::~Tool() {
        if ( _conn )
            delete _conn;
    }

    MONGO_INITIALIZER(ToolMocks)(InitializerContext*) {
        setGlobalAuthorizationManager(new AuthorizationManager(
                new AuthzManagerExternalStateMock()));
        repl::ReplSettings replSettings;
        repl::setGlobalReplicationCoordinator(new repl::ReplicationCoordinatorMock(replSettings));
        return Status::OK();
    }

    int Tool::main( int argc , char ** argv, char ** envp ) {
        setupSignalHandlers(true);

        static StaticObserver staticObserver;

        mongo::runGlobalInitializersOrDie(argc, argv, envp);

        // hide password from ps output
        for (int i=0; i < (argc-1); ++i) {
            if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--password")) {
                char* arg = argv[i+1];
                while (*arg) {
                    *arg++ = 'x';
                }
            }
        }

        if (!toolGlobalParams.useDirectClient) {
            if (toolGlobalParams.noconnection) {
                // do nothing
            }
            else {
                string errmsg;

                ConnectionString cs = ConnectionString::parse(toolGlobalParams.connectionString,
                                                              errmsg);
                if ( ! cs.isValid() ) {
                    toolError() << "invalid hostname [" << toolGlobalParams.connectionString << "] "
                              << errmsg << std::endl;
                    quickExit(-1);
                }

                _conn = cs.connect( errmsg );
                if ( ! _conn ) {
                    toolError() << "couldn't connect to [" << toolGlobalParams.connectionString
                              << "] " << errmsg << std::endl;
                    quickExit(-1);
                }

                toolInfoOutput() << "connected to: " << toolGlobalParams.connectionString
                                 << std::endl;
            }

        }
        else {
            verify( lastError.get( true ) );

            Client::initThread("tools");
            storageGlobalParams.dbpath = toolGlobalParams.dbpath;
            try {
                getGlobalEnvironment()->setGlobalStorageEngine(storageGlobalParams.engine);
            }
            catch (const DBException& ex) {
                if (ex.getCode() == ErrorCodes::DBPathInUse) {
                    toolError() << std::endl << "If you are running a mongod on the same "
                                 "path you should connect to that instead of direct data "
                                  "file access" << std::endl << std::endl;
                }
                else {
                    toolError() << "Failed to initialize storage engine: " << ex.toString();
                }
                dbexit( EXIT_FS );
                quickExit(EXIT_FAILURE);
            }

            _txn.reset(new OperationContextImpl());
            _conn = new DBDirectClient(_txn.get());
        }

        int ret = -1;
        try {
            if (!toolGlobalParams.useDirectClient && !toolGlobalParams.noconnection)
                auth();
            ret = run();
        }
        catch ( DBException& e ) {
            toolError() << "assertion: " << e.toString() << std::endl;
            ret = -1;
        }
        catch(const boost::filesystem::filesystem_error &fse) {
            /*
              https://jira.mongodb.org/browse/SERVER-2904

              Simple tools that don't access the database, such as
              bsondump, aren't throwing DBExceptions, but are throwing
              boost exceptions.

              The currently available set of error codes don't seem to match
              boost documentation.  boost::filesystem::not_found_error
              (from http://www.boost.org/doc/libs/1_31_0/libs/filesystem/doc/exception.htm)
              doesn't seem to exist in our headers.  Also, fse.code() isn't
              boost::system::errc::no_such_file_or_directory when this
              happens, as you would expect.  And, determined from
              experimentation that the command-line argument gets turned into
              "\\?" instead of "/?" !!!
             */
#if defined(_WIN32)
            if (/*(fse.code() == boost::system::errc::no_such_file_or_directory) &&*/
                (fse.path1() == "\\?"))
                printHelp(cerr);
            else
#endif // _WIN32
                toolError() << "error: " << fse.what() << std::endl;

            ret = -1;
        }

        if ( currentClient.get() )
            currentClient.get()->shutdown();

        if (toolGlobalParams.useDirectClient) {
            exitCleanly(EXIT_CLEAN);
        }

        fflush(stdout);
        fflush(stderr);
        quickExit(ret);
    }

    DBClientBase& Tool::conn( bool slaveIfPaired ) {
        if ( slaveIfPaired && _conn->type() == ConnectionString::SET ) {
            if (!_slaveConn) {
                DBClientReplicaSet* rs = static_cast<DBClientReplicaSet*>(_conn);
                _slaveConn = &rs->slaveConn();
            }
            return *_slaveConn;
        }
        return *_conn;
    }

    bool Tool::isMaster() {
        if (toolGlobalParams.useDirectClient) {
            return true;
        }

        BSONObj info;
        bool isMaster;
        bool ok = conn().isMaster(isMaster, &info);

        if (ok && !isMaster) {
            toolError() << "ERROR: trying to write to non-master " << conn().toString()
                        << std::endl;
            toolError() << "isMaster info: " << info << std::endl;
            return false;
        }

        return true;
    }

    bool Tool::isMongos() {
        // TODO: when mongos supports QueryOption_Exaust add a version check (SERVER-2628)
        BSONObj isdbgrid;
        conn("true").simpleCommand("admin", &isdbgrid, "isdbgrid");
        return isdbgrid["isdbgrid"].trueValue();
    }

    std::string Tool::getAuthenticationDatabase() {
        if (!toolGlobalParams.authenticationDatabase.empty()) {
            return toolGlobalParams.authenticationDatabase;
        }

        if (!toolGlobalParams.db.empty()) {
            return toolGlobalParams.db;
        }

        return "admin";
    }

    /**
     * Validate authentication on the server for the given dbname.
     */
    void Tool::auth() {

        if (toolGlobalParams.username.empty()) {
            // Make sure that we don't need authentication to connect to this db
            // findOne throws an AssertionException if it's not authenticated.
            if (toolGlobalParams.coll.size() > 0) {
                // BSONTools don't have a collection
                conn().findOne(getNS(), Query("{}"), 0, QueryOption_SlaveOk);
            }

            return;
        }

        BSONObjBuilder authParams;
        authParams <<
            saslCommandUserDBFieldName << getAuthenticationDatabase() <<
            saslCommandUserFieldName << toolGlobalParams.username <<
            saslCommandPasswordFieldName << toolGlobalParams.password  <<
            saslCommandMechanismFieldName <<
            toolGlobalParams.authenticationMechanism;

        if (!toolGlobalParams.gssapiServiceName.empty()) {
            authParams << saslCommandServiceNameFieldName << toolGlobalParams.gssapiServiceName;
        }

        if (!toolGlobalParams.gssapiHostName.empty()) {
            authParams << saslCommandServiceHostnameFieldName << toolGlobalParams.gssapiHostName;
        }

        _conn->auth(authParams.obj());
    }

    BSONTool::BSONTool() : Tool() { }

    int BSONTool::run() {

        if (bsonToolGlobalParams.hasFilter) {
            _matcher.reset(new Matcher(fromjson(bsonToolGlobalParams.filter),
                                       MatchExpressionParser::WhereCallback()));
        }

        return doRun();
    }

    long long BSONTool::processFile(const boost::filesystem::path& root, std::ostream* out) {
        bool isFifoFile = boost::filesystem::status(root).type() == boost::filesystem::fifo_file;
        bool isStdin = root == "-";

        std::string fileName = root.string();

        unsigned long long fileLength = (isFifoFile || isStdin) ?
            std::numeric_limits<unsigned long long>::max() : file_size(root);

        if ( fileLength == 0 ) {
            toolInfoOutput() << "file " << fileName << " empty, skipping" << std::endl;
            return 0;
        }


        FILE* file = isStdin ? stdin : fopen( fileName.c_str() , "rb" );
        if ( ! file ) {
            toolError() << "error opening file: " << fileName << " " << errnoWithDescription()
                      << std::endl;
            return 0;
        }

        if (!isFifoFile && !isStdin) {
#ifdef POSIX_FADV_SEQUENTIAL
            posix_fadvise(fileno(file), 0, fileLength, POSIX_FADV_SEQUENTIAL);
#endif

            if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1))) {
                toolInfoOutput() << "\t file size: " << fileLength << std::endl;
            }
        }

        unsigned long long read = 0;
        unsigned long long num = 0;
        unsigned long long processed = 0;

        const int BUF_SIZE = BSONObjMaxUserSize + ( 1024 * 1024 );
        boost::scoped_array<char> buf_holder(new char[BUF_SIZE]);
        char * buf = buf_holder.get();

        // no progress is available for FIFO
        // only for regular files
        boost::scoped_ptr<ProgressMeter> m;
        if (!toolGlobalParams.quiet && !isFifoFile && !isStdin) {
            m.reset(new ProgressMeter( fileLength ));
            m->setUnits( "bytes" );
        }

        while ( read < fileLength ) {
            size_t amt = fread(buf, 1, 4, file);
            // end of fifo
            if ((isFifoFile || isStdin) && ::feof(file)) {
                break;
            }
            verify( amt == 4 );

            int size = ((int*)buf)[0];
            uassert( 10264 , str::stream() << "invalid object size: " << size , size < BUF_SIZE );

            amt = fread(buf+4, 1, size-4, file);
            verify( amt == (size_t)( size - 4 ) );

            BSONObj o( buf );
            if (bsonToolGlobalParams.objcheck) {
                const Status status = validateBSON(buf, size);
                if (!status.isOK()) {
                    toolError() << "INVALID OBJECT - going to try and print out " << std::endl;
                    toolError() << "size: " << size << std::endl;
                    toolError() << "error: " << status.reason() << std::endl;

                    StringBuilder sb;
                    try {
                        o.toString(sb); // using StringBuilder version to get as much as possible
                    } catch (...) {
                        toolError() << "object up to error: " << sb.str() << endl;
                        throw;
                    }
                    toolError() << "complete object: " << sb.str() << endl;

                    // NOTE: continuing with object even though we know it is invalid.
                }
            }

            if (!bsonToolGlobalParams.hasFilter || _matcher->matches(o)) {
                gotObject(o, out);
                processed++;
            }

            read += o.objsize();
            num++;

            if (m.get()) {
                m->hit(o.objsize());
            }
        }

        if (!isStdin) {
            fclose(file);
        }

        if (!isFifoFile && !isStdin) {
            uassert(10265, "counts don't match", read == fileLength);
        }
        toolInfoOutput() << num << ((num == 1) ? " document" : " documents")
                         << " found" << std::endl;
        if (bsonToolGlobalParams.hasFilter) {
            toolInfoOutput() << processed
                             << ((processed == 1) ? " document" : " documents")
                             << " processed" << std::endl;
        }
        return processed;
    }

}

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables toolMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    mongo::WindowsCommandLine wcl(argc, argvW, envpW);
    auto_ptr<Tool> instance = (*Tool::createInstance)();
    int exitCode = instance->main(argc, wcl.argv(), wcl.envp());
    quickExit(exitCode);
}

#else
int main(int argc, char* argv[], char** envp) {
    auto_ptr<Tool> instance = (*Tool::createInstance)();
    quickExit(instance->main(argc, argv, envp));
}
#endif
