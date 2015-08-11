// mongo/shell/shell_utils_launcher.cpp
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

#include "mongo/shell/shell_utils_launcher.h"

#include <iostream>
#include <map>
#include <signal.h>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define SIGKILL 9
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#include "mongo/client/dbclientinterface.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_win32.h"

namespace mongo {

using std::unique_ptr;
using std::cout;
using std::endl;
using std::make_pair;
using std::map;
using std::pair;
using std::string;
using std::stringstream;
using std::vector;

extern bool dbexitCalled;

#ifdef _WIN32
inline int close(int fd) {
    return _close(fd);
}
inline int read(int fd, void* buf, size_t size) {
    return _read(fd, buf, size);
}
inline int pipe(int fds[2]) {
    return _pipe(fds, 4096, _O_TEXT | _O_NOINHERIT);
}
#endif

/**
 * These utilities are thread safe but do not provide mutually exclusive access to resources
 * identified by the caller.  Resources identified by a pid or port should not be accessed
 * by different threads.  Dependent filesystem paths should not be accessed by different
 * threads.
 */
namespace shell_utils {

ProgramOutputMultiplexer programOutputLogger;

bool ProgramRegistry::isPortRegistered(int port) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    return _ports.count(port) == 1;
}

ProcessId ProgramRegistry::pidForPort(int port) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    verify(isPortRegistered(port));
    return _ports.find(port)->second.first;
}

int ProgramRegistry::portForPid(ProcessId pid) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    for (map<int, pair<ProcessId, int>>::const_iterator it = _ports.begin(); it != _ports.end();
         ++it) {
        if (it->second.first == pid)
            return it->first;
    }

    return -1;
}

void ProgramRegistry::registerPort(int port, ProcessId pid, int output) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    verify(!isPortRegistered(port));
    _ports.insert(make_pair(port, make_pair(pid, output)));
}

void ProgramRegistry::deletePort(int port) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    if (!isPortRegistered(port)) {
        return;
    }
    close(_ports.find(port)->second.second);
    _ports.erase(port);
}

void ProgramRegistry::getRegisteredPorts(vector<int>& ports) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    for (map<int, pair<ProcessId, int>>::const_iterator i = _ports.begin(); i != _ports.end();
         ++i) {
        ports.push_back(i->first);
    }
}

bool ProgramRegistry::isPidRegistered(ProcessId pid) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    return _pids.count(pid) == 1;
}

void ProgramRegistry::registerPid(ProcessId pid, int output) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    verify(!isPidRegistered(pid));
    _pids.insert(make_pair(pid, output));
}

void ProgramRegistry::deletePid(ProcessId pid) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    if (!isPidRegistered(pid)) {
        int port = portForPid(pid);
        if (port < 0)
            return;
        deletePort(port);
        return;
    }
    close(_pids.find(pid)->second);
    _pids.erase(pid);
}

void ProgramRegistry::getRegisteredPids(vector<ProcessId>& pids) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    for (map<ProcessId, int>::const_iterator i = _pids.begin(); i != _pids.end(); ++i) {
        pids.push_back(i->first);
    }
}

ProgramRegistry& registry = *(new ProgramRegistry());

void goingAwaySoon() {
    stdx::lock_guard<stdx::mutex> lk(mongoProgramOutputMutex);
    mongo::dbexitCalled = true;
}

void ProgramOutputMultiplexer::appendLine(int port, ProcessId pid, const char* line) {
    stdx::lock_guard<stdx::mutex> lk(mongoProgramOutputMutex);
    uassert(28695, "program is terminating", !mongo::dbexitCalled);
    stringstream buf;
    if (port > 0)
        buf << " m" << port << "| " << line;
    else
        buf << "sh" << pid << "| " << line;
    printf("%s\n", buf.str().c_str());  // cout << buf.str() << endl;
    fflush(stdout);  // not implicit if stdout isn't directly outputting to a console.
    _buffer << buf.str() << endl;
}

string ProgramOutputMultiplexer::str() const {
    stdx::lock_guard<stdx::mutex> lk(mongoProgramOutputMutex);
    string ret = _buffer.str();
    size_t len = ret.length();
    if (len > 100000) {
        ret = ret.substr(len - 100000, 100000);
    }
    return ret;
}

void ProgramOutputMultiplexer::clear() {
    stdx::lock_guard<stdx::mutex> lk(mongoProgramOutputMutex);
    _buffer.str("");
}

ProgramRunner::ProgramRunner(const BSONObj& args) {
    verify(!args.isEmpty());

    string program(args.firstElement().valuestrsafe());
    verify(!program.empty());
    boost::filesystem::path programPath = findProgram(program);

    string prefix("mongod-");
    bool isMongodProgram =
        string("mongod") == program || program.compare(0, prefix.size(), prefix) == 0;

    prefix = "mongos-";
    bool isMongosProgram =
        string("mongos") == program || program.compare(0, prefix.size(), prefix) == 0;

#if 0
            if (isMongosProgram == "mongos") {
                _argv.push_back("valgrind");
                _argv.push_back("--log-file=/tmp/mongos-%p.valgrind");
                _argv.push_back("--leak-check=yes");
                _argv.push_back("--suppressions=valgrind.suppressions");
                //_argv.push_back("--error-exitcode=1");
                _argv.push_back("--");
            }
#endif

    _argv.push_back(programPath.string());

    _port = -1;

    BSONObjIterator j(args);
    j.next();  // skip program name (handled above)
    while (j.more()) {
        BSONElement e = j.next();
        string str;
        if (e.isNumber()) {
            stringstream ss;
            ss << e.number();
            str = ss.str();
        } else {
            verify(e.type() == mongo::String);
            str = e.valuestr();
        }
        if (str == "--port")
            _port = -2;
        else if (_port == -2)
            _port = strtol(str.c_str(), 0, 10);
        _argv.push_back(str);
    }

    if (!isMongodProgram && !isMongosProgram && program != "mongobridge")
        _port = 0;
    else {
        if (_port <= 0)
            log() << "error: a port number is expected when running " << program
                  << " from the shell" << endl;
        verify(_port > 0);
    }
    if (_port > 0) {
        bool haveDbForPort = registry.isPortRegistered(_port);
        if (haveDbForPort) {
            log() << "already have db for port: " << _port << endl;
            verify(!haveDbForPort);
        }
    }
}

void ProgramRunner::start() {
    int pipeEnds[2];
    int status = pipe(pipeEnds);
    if (status != 0) {
        error() << "failed to create pipe: " << errnoWithDescription() << endl;
        fassertFailed(16701);
    }

    fflush(0);
    launchProcess(pipeEnds[1]);  // sets _pid

    {
        stringstream ss;
        ss << "shell: started program (sh" << _pid << "): ";
        for (unsigned i = 0; i < _argv.size(); i++) {
            ss << " " << _argv[i];
        }
        log() << ss.str() << endl;
    }

    if (_port > 0)
        registry.registerPort(_port, _pid, pipeEnds[1]);
    else
        registry.registerPid(_pid, pipeEnds[1]);
    _pipe = pipeEnds[0];
}

void ProgramRunner::operator()() {
    try {
        // This assumes there aren't any 0's in the mongo program output.
        // Hope that's ok.
        const unsigned bufSize = 128 * 1024;
        char buf[bufSize];
        char temp[bufSize];
        char* start = buf;
        while (1) {
            int lenToRead = (bufSize - 1) - (start - buf);
            if (lenToRead <= 0) {
                log() << "error: lenToRead: " << lenToRead << endl;
                log() << "first 300: " << string(buf, 0, 300) << endl;
            }
            verify(lenToRead > 0);
            int ret = read(_pipe, (void*)start, lenToRead);
            if (mongo::dbexitCalled)
                break;
            verify(ret != -1);
            start[ret] = '\0';
            if (strlen(start) != unsigned(ret))
                programOutputLogger.appendLine(
                    _port, _pid, "WARNING: mongod wrote null bytes to output");
            char* last = buf;
            for (char* i = strchr(buf, '\n'); i; last = i + 1, i = strchr(last, '\n')) {
                *i = '\0';
                programOutputLogger.appendLine(_port, _pid, last);
            }
            if (ret == 0) {
                if (*last)
                    programOutputLogger.appendLine(_port, _pid, last);
                close(_pipe);
                break;
            }
            if (last != buf) {
                strncpy(temp, last, bufSize);
                temp[bufSize - 1] = '\0';
                strncpy(buf, temp, bufSize);
                buf[bufSize - 1] = '\0';
            } else {
                verify(strlen(buf) < bufSize);
            }
            start = buf + strlen(buf);
        }
    } catch (...) {
    }
}

boost::filesystem::path ProgramRunner::findProgram(const string& prog) {
    boost::filesystem::path p = prog;

#ifdef _WIN32
    // The system programs either come versioned in the form of <utility>-<major.minor>
    // (e.g., mongorestore-2.4) or just <utility>. For windows, the appropriate extension
    // needs to be appended.
    //
    if (p.extension() != ".exe") {
        p = prog + ".exe";
    }
#endif

    if (boost::filesystem::exists(p)) {
#ifndef _WIN32
        p = boost::filesystem::initial_path() / p;
#endif
        return p;
    }

    {
        boost::filesystem::path t = boost::filesystem::current_path() / p;
        if (boost::filesystem::exists(t))
            return t;
    }

    {
        boost::filesystem::path t = boost::filesystem::initial_path() / p;
        if (boost::filesystem::exists(t))
            return t;
    }

    return p;  // not found; might find via system path
}

void ProgramRunner::launchProcess(int child_stdout) {
#ifdef _WIN32
    stringstream ss;
    for (unsigned i = 0; i < _argv.size(); i++) {
        if (i)
            ss << ' ';
        if (_argv[i].find(' ') == string::npos)
            ss << _argv[i];
        else {
            ss << '"';
            // escape all embedded quotes
            for (size_t j = 0; j < _argv[i].size(); ++j) {
                if (_argv[i][j] == '"')
                    ss << '\\';
                ss << _argv[i][j];
            }
            ss << '"';
        }
    }

    string args = ss.str();

    std::unique_ptr<TCHAR[]> args_tchar(new TCHAR[args.size() + 1]);
    size_t i;
    for (i = 0; i < args.size(); i++)
        args_tchar[i] = args[i];
    args_tchar[i] = 0;

    HANDLE h = (HANDLE)_get_osfhandle(child_stdout);
    verify(h != INVALID_HANDLE_VALUE);
    verify(SetHandleInformation(h, HANDLE_FLAG_INHERIT, 1));

    STARTUPINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = h;
    si.hStdOutput = h;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    bool success =
        CreateProcess(NULL, args_tchar.get(), NULL, NULL, true, 0, NULL, NULL, &si, &pi) != 0;
    if (!success) {
        LPSTR lpMsgBuf = 0;
        DWORD dw = GetLastError();
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       dw,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       (LPSTR)&lpMsgBuf,
                       0,
                       NULL);
        stringstream ss;
        ss << "couldn't start process " << _argv[0] << "; " << lpMsgBuf;
        uassert(14042, ss.str(), success);
        LocalFree(lpMsgBuf);
    }

    CloseHandle(pi.hThread);

    _pid = ProcessId::fromNative(pi.dwProcessId);
    registry._handles.insert(make_pair(_pid, pi.hProcess));

#else

    unique_ptr<const char* []> argvStorage(new const char* [_argv.size() + 1]);
    const char** argv = argvStorage.get();
    for (unsigned i = 0; i < _argv.size(); i++) {
        argv[i] = _argv[i].c_str();
    }
    argv[_argv.size()] = 0;

    unique_ptr<const char* []> envStorage(new const char* [2]);
    const char** env = envStorage.get();
    env[0] = NULL;
    env[1] = NULL;

    pid_t nativePid = fork();
    _pid = ProcessId::fromNative(nativePid);
    // Async signal unsafe functions should not be called in the child process.

    verify(nativePid != -1);
    if (nativePid == 0) {
        // DON'T ASSERT IN THIS BLOCK - very bad things will happen

        if (dup2(child_stdout, STDOUT_FILENO) == -1 || dup2(child_stdout, STDERR_FILENO) == -1) {
            // Async signal unsafe code reporting a terminal error condition.
            cout << "Unable to dup2 child output: " << errnoWithDescription() << endl;
            quickExit(-1);  // do not pass go, do not call atexit handlers
        }

        // NOTE execve is async signal safe, but it is not clear that execvp is async
        // signal safe.
        execvp(argv[0], const_cast<char**>(argv));

        // Async signal unsafe code reporting a terminal error condition.
        cout << "Unable to start program " << argv[0] << ' ' << errnoWithDescription() << endl;
        quickExit(-1);
    }

#endif
}

// returns true if process exited
bool wait_for_pid(ProcessId pid, bool block = true, int* exit_code = NULL) {
#ifdef _WIN32
    verify(registry._handles.count(pid));
    HANDLE h = registry._handles[pid];

    if (block) {
        if (WaitForSingleObject(h, INFINITE)) {
            log() << "WaitForSingleObject failed: " << errnoWithDescription();
        }
    }

    DWORD tmp;
    if (GetExitCodeProcess(h, &tmp)) {
        if (tmp == STILL_ACTIVE) {
            if (block)
                log() << "Process is STILL_ACTIVE even after blocking";
            return false;
        }
        CloseHandle(h);
        registry._handles.erase(pid);
        if (exit_code)
            *exit_code = tmp;
        return true;
    } else {
        log() << "GetExitCodeProcess failed: " << errnoWithDescription();
        return false;
    }
#else
    int tmp;
    bool ret = (pid.toNative() == waitpid(pid.toNative(), &tmp, (block ? 0 : WNOHANG)));
    if (ret && exit_code) {
        if (WIFEXITED(tmp)) {
            *exit_code = WEXITSTATUS(tmp);
        } else if (WIFSIGNALED(tmp)) {
            *exit_code = -WTERMSIG(tmp);
        } else {
            MONGO_UNREACHABLE;
        }
    }
    return ret;

#endif
}

BSONObj RawMongoProgramOutput(const BSONObj& args, void* data) {
    return BSON("" << programOutputLogger.str());
}

BSONObj ClearRawMongoProgramOutput(const BSONObj& args, void* data) {
    programOutputLogger.clear();
    return undefinedReturn;
}

BSONObj CheckProgram(const BSONObj& args, void* data) {
    ProcessId pid = ProcessId::fromNative(singleArg(args).numberInt());
    bool isDead = wait_for_pid(pid, false);
    if (isDead)
        registry.deletePid(pid);
    return BSON(string("") << (!isDead));
}

BSONObj WaitProgram(const BSONObj& a, void* data) {
    ProcessId pid = ProcessId::fromNative(singleArg(a).numberInt());
    int exit_code = -123456;  // sentinel value
    wait_for_pid(pid, true, &exit_code);
    registry.deletePid(pid);
    return BSON(string("") << exit_code);
}

BSONObj StartMongoProgram(const BSONObj& a, void* data) {
    _nokillop = true;
    ProgramRunner r(a);
    r.start();
    stdx::thread t(r);
    t.detach();
    return BSON(string("") << r.pid().asLongLong());
}

BSONObj RunMongoProgram(const BSONObj& a, void* data) {
    ProgramRunner r(a);
    r.start();
    stdx::thread t(r);
    t.detach();
    int exit_code = -123456;  // sentinel value
    wait_for_pid(r.pid(), true, &exit_code);
    if (r.port() > 0) {
        registry.deletePort(r.port());
    } else {
        registry.deletePid(r.pid());
    }
    return BSON(string("") << exit_code);
}

BSONObj RunProgram(const BSONObj& a, void* data) {
    ProgramRunner r(a);
    r.start();
    stdx::thread t(r);
    t.detach();
    int exit_code = -123456;  // sentinel value
    wait_for_pid(r.pid(), true, &exit_code);
    registry.deletePid(r.pid());
    return BSON(string("") << exit_code);
}

BSONObj ResetDbpath(const BSONObj& a, void* data) {
    verify(a.nFields() == 1);
    string path = a.firstElement().valuestrsafe();
    verify(!path.empty());
    if (boost::filesystem::exists(path))
        boost::filesystem::remove_all(path);
    boost::filesystem::create_directory(path);
    return undefinedReturn;
}

BSONObj PathExists(const BSONObj& a, void* data) {
    verify(a.nFields() == 1);
    string path = a.firstElement().valuestrsafe();
    verify(!path.empty());
    bool exists = boost::filesystem::exists(path);
    return BSON(string("") << exists);
}

void copyDir(const boost::filesystem::path& from, const boost::filesystem::path& to) {
    boost::filesystem::directory_iterator end;
    boost::filesystem::directory_iterator i(from);
    while (i != end) {
        boost::filesystem::path p = *i;
        if (p.leaf() != "mongod.lock" && p.leaf() != "WiredTiger.lock") {
            if (boost::filesystem::is_directory(p)) {
                boost::filesystem::path newDir = to / p.leaf();
                boost::filesystem::create_directory(newDir);
                copyDir(p, newDir);
            } else {
                boost::filesystem::copy_file(p, to / p.leaf());
            }
        }
        ++i;
    }
}

// NOTE target dbpath will be cleared first
BSONObj CopyDbpath(const BSONObj& a, void* data) {
    verify(a.nFields() == 2);
    BSONObjIterator i(a);
    string from = i.next().str();
    string to = i.next().str();
    verify(!from.empty());
    verify(!to.empty());
    if (boost::filesystem::exists(to))
        boost::filesystem::remove_all(to);
    boost::filesystem::create_directory(to);
    copyDir(from, to);
    return undefinedReturn;
}

inline void kill_wrapper(ProcessId pid, int sig, int port, const BSONObj& opt) {
#ifdef _WIN32
    if (sig == SIGKILL || port == 0) {
        verify(registry._handles.count(pid));
        TerminateProcess(registry._handles[pid], 1);  // returns failure for "zombie" processes.
        return;
    }

    std::string eventName = getShutdownSignalName(pid.asUInt32());

    HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (event == NULL) {
        int gle = GetLastError();
        if (gle != ERROR_FILE_NOT_FOUND) {
            warning() << "kill_wrapper OpenEvent failed: " << errnoWithDescription();
        } else {
            log() << "kill_wrapper OpenEvent failed to open event to the process " << pid.asUInt32()
                  << ". It has likely died already or server is running an older version."
                  << " Attempting to shutdown through admin command.";

            // Back-off to the old way of shutting down the server on Windows, in case we
            // are managing a pre-2.6.0rc0 service, which did not have the event.
            //
            try {
                DBClientConnection conn;
                conn.connect(HostAndPort{"127.0.0.1:" + BSONObjBuilder::numStr(port)});

                BSONElement authObj = opt["auth"];

                if (!authObj.eoo()) {
                    string errMsg;
                    conn.auth("admin", authObj["user"].String(), authObj["pwd"].String(), errMsg);

                    if (!errMsg.empty()) {
                        cout << "Failed to authenticate before shutdown: " << errMsg << endl;
                    }
                }

                BSONObj info;
                BSONObjBuilder b;
                b.append("shutdown", 1);
                b.append("force", 1);
                conn.runCommand("admin", b.done(), info);
            } catch (...) {
                // Do nothing. This command never returns data to the client and the driver
                // doesn't like that.
                //
            }
        }
        return;
    }

    ON_BLOCK_EXIT(CloseHandle, event);

    bool result = SetEvent(event);
    if (!result) {
        error() << "kill_wrapper SetEvent failed: " << errnoWithDescription();
        return;
    }
#else
    int x = kill(pid.toNative(), sig);
    if (x) {
        if (errno == ESRCH) {
        } else {
            log() << "killFailed: " << errnoWithDescription() << endl;
            verify(x == 0);
        }
    }

#endif
}

int killDb(int port, ProcessId _pid, int signal, const BSONObj& opt) {
    ProcessId pid;
    int exitCode = 0;
    if (port > 0) {
        if (!registry.isPortRegistered(port)) {
            log() << "No db started on port: " << port << endl;
            return 0;
        }
        pid = registry.pidForPort(port);
    } else {
        pid = _pid;
    }

    kill_wrapper(pid, signal, port, opt);

    int i = 0;
    for (; i < 130; ++i) {
        if (i == 60) {
            log() << "process on port " << port << ", with pid " << pid
                  << " not terminated, sending sigkill" << endl;
            kill_wrapper(pid, SIGKILL, port, opt);
        }
        if (wait_for_pid(pid, false, &exitCode))
            break;
        sleepmillis(1000);
    }
    if (i == 130) {
        log() << "failed to terminate process on port " << port << ", with pid " << pid << endl;
        verify("Failed to terminate process" == 0);
    }

    if (port > 0) {
        registry.deletePort(port);
    } else {
        registry.deletePid(pid);
    }
    // FIXME I think the intention here is to do an extra sleep only when SIGKILL is sent to the
    // child process. We may want to change the 4 below to 29, since values of i greater than that
    // indicate we sent a SIGKILL.
    if (i > 4 || signal == SIGKILL) {
        sleepmillis(4000);  // allow operating system to reclaim resources
    }

    return exitCode;
}

int killDb(int port, ProcessId _pid, int signal) {
    BSONObj dummyOpt;
    return killDb(port, _pid, signal, dummyOpt);
}

int getSignal(const BSONObj& a) {
    int ret = SIGTERM;
    if (a.nFields() >= 2) {
        BSONObjIterator i(a);
        i.next();
        BSONElement e = i.next();
        verify(e.isNumber());
        ret = int(e.number());
    }
    return ret;
}

BSONObj getStopMongodOpts(const BSONObj& a) {
    if (a.nFields() == 3) {
        BSONObjIterator i(a);
        i.next();
        i.next();
        BSONElement e = i.next();

        if (e.isABSONObj()) {
            return e.embeddedObject();
        }
    }

    return BSONObj();
}

/** stopMongoProgram(port[, signal]) */
BSONObj StopMongoProgram(const BSONObj& a, void* data) {
    int nFields = a.nFields();
    verify(nFields >= 1 && nFields <= 3);
    uassert(15853, "stopMongo needs a number", a.firstElement().isNumber());
    int port = int(a.firstElement().number());
    int code = killDb(port, ProcessId::fromNative(0), getSignal(a), getStopMongodOpts(a));
    log() << "shell: stopped mongo program on port " << port << endl;
    return BSON("" << (double)code);
}

BSONObj StopMongoProgramByPid(const BSONObj& a, void* data) {
    verify(a.nFields() == 1 || a.nFields() == 2);
    uassert(15852, "stopMongoByPid needs a number", a.firstElement().isNumber());
    ProcessId pid = ProcessId::fromNative(int(a.firstElement().number()));
    int code = killDb(0, pid, getSignal(a));
    log() << "shell: stopped mongo program on pid " << pid << endl;
    return BSON("" << (double)code);
}

void KillMongoProgramInstances() {
    vector<int> ports;
    registry.getRegisteredPorts(ports);
    for (vector<int>::iterator i = ports.begin(); i != ports.end(); ++i)
        killDb(*i, ProcessId::fromNative(0), SIGTERM);
    vector<ProcessId> pids;
    registry.getRegisteredPids(pids);
    for (vector<ProcessId>::iterator i = pids.begin(); i != pids.end(); ++i)
        killDb(0, *i, SIGTERM);
}

MongoProgramScope::~MongoProgramScope() {
    DESTRUCTOR_GUARD(KillMongoProgramInstances(); ClearRawMongoProgramOutput(BSONObj(), 0);)
}

void installShellUtilsLauncher(Scope& scope) {
    scope.injectNative("_startMongoProgram", StartMongoProgram);
    scope.injectNative("runProgram", RunProgram);
    scope.injectNative("run", RunProgram);
    scope.injectNative("_runMongoProgram", RunMongoProgram);
    scope.injectNative("_stopMongoProgram", StopMongoProgram);
    scope.injectNative("stopMongoProgramByPid", StopMongoProgramByPid);
    scope.injectNative("rawMongoProgramOutput", RawMongoProgramOutput);
    scope.injectNative("clearRawMongoProgramOutput", ClearRawMongoProgramOutput);
    scope.injectNative("waitProgram", WaitProgram);
    scope.injectNative("checkProgram", CheckProgram);
    scope.injectNative("resetDbpath", ResetDbpath);
    scope.injectNative("pathExists", PathExists);
    scope.injectNative("copyDbpath", CopyDbpath);
}
}
}
