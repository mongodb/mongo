/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/process_id.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

#include <iosfwd>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace mongo::shell_utils {
class ProgramRunner;

/** Reads log lines from programs and optionally log them. All public members are thread safe, so
 *  a single instance can be used to record logs of many concurrently-running programs.
 */
class ProgramOutputMultiplexer {
public:
    void appendLine(
        int port, ProcessId pid, const std::string& name, const std::string& line, bool shouldLog);
    std::string str() const;
    void clear();

private:
    std::stringstream _buffer;
    mutable stdx::mutex _mongoProgramOutputMutex;
};

/**
 * A registry of spawned programs that are identified by a bound port or else a system pid.
 * All public member functions are thread safe. It also owns the global ProgramOutputMultiplexer.
 */
class ProgramRegistry {
public:
    static void create(ServiceContext* serviceContext);

    static ProgramRegistry* get(ServiceContext* serviceContext);

    ProgramOutputMultiplexer* getProgramOutputMultiplexer();

    /** Create a ProgramRunner instance and return a handle to it. */
    ProgramRunner createProgramRunner(BSONObj args,
                                      BSONObj env,
                                      bool isMongo,
                                      const std::string& loggingPrefix);

    bool isPortRegistered(int port) const;
    /** @return pid for a registered port. */
    ProcessId pidForPort(int port) const;
    /** @return port (-1 if doesn't exist) for a registered pid. */
    int portForPid(ProcessId pid) const;
    /** Register an unregistered program. */
    void registerProgram(ProcessId pid, int port = -1);
    /** Registers the reader thread for the PID. Must be called before `joinReaderThread`. */
    void registerReaderThread(ProcessId pid, stdx::thread reader);
    /** Closes the registered program's write pipe and waits for all of the written output to be
     * consumed by the reader thread, then removes the program from the registry */
    void unregisterProgram(ProcessId pid);

    bool isPidRegistered(ProcessId pid) const;
    /** platform-agnostic wrapper around waitpid that automatically cleans up
     * the program registry
     * @param pid the processid
     * @param block if true, block the thread until the child has exited
     * @param exit_code[out] if set, and an exit code is available, the code will be stored here
     * @return true if the process has exited, false otherwise */
    bool waitForPid(ProcessId pid, bool block, int* exit_code = nullptr);
    /** check if a child process is alive. Never blocks
     * @param pid the processid
     * @param exit_code[out] if set, and an exit code is available, the code will be stored here
     * @return true if the process has exited, false otherwise */
    bool isPidDead(ProcessId pids, int* exit_code = nullptr);
    void getRegisteredPorts(std::vector<int>& ports);
    void getRegisteredPids(std::vector<ProcessId>& pids);
    /** Return a list of every processids ever registered. */
    std::vector<ProcessId> getRegisteredPidsHistory();

private:
    void updatePidExitCode(ProcessId pid, int exitCode);

private:
    friend class ProgramRunner;

    stdx::unordered_set<ProcessId> _registeredPids;
    std::vector<ProcessId> _registeredPidsHistory;
    stdx::unordered_map<int, ProcessId> _portToPidMap;
    stdx::unordered_map<ProcessId, int> _pidToExitCode;
    stdx::unordered_map<ProcessId, stdx::thread> _outputReaderThreads;
    ProgramOutputMultiplexer _programOutputMultiplexer;
    mutable stdx::recursive_mutex _mutex;
    mutable stdx::mutex _createProcessMtx;

#ifdef _WIN32
private:
    std::map<ProcessId, HANDLE> _handles;

public:
    /** Will uassert with ErrorCodes::BadValue if the pid is unregistered. */
    HANDLE getHandleForPid(ProcessId pid) const;
    void eraseHandleForPid(ProcessId pid);
    void insertHandleForPid(ProcessId pid, HANDLE handle);

#endif
};

/** Helper class for launching a program and logging its output. */
class ProgramRunner {
public:
    /** Launch the program. */
    void start(bool shouldLogArgs = true);

    /** Reads the program's output into the provided instance of ProgramOutputMultiplexer.
     *  Note that the passed-in multiplexer will typically be the global programOutputLogger so that
     *  all programs' outputs can be logged concurrently. If the multiplexer should not
     *  log this program's output to standard output, then set shouldLogOutput to false.
     */
    void operator()(ProgramOutputMultiplexer* multiplexer, bool shouldLogOutput);

    ProcessId pid() const {
        return _pid;
    }
    int port() const {
        return _port;
    }

private:
    friend class ProgramRegistry;

    /** @param args The program's arguments, including the program name.
     *  @param env Environment to run the program with, which will override any set by the local
     *             environment
     * @param isMongo Indicator variable, true if runs as a mongo process.
     */
    ProgramRunner(BSONObj args,
                  BSONObj env,
                  bool isMongo,
                  const std::string& loggingPrefix,
                  ProgramRegistry* registry);

    boost::filesystem::path findProgram(const std::string& prog);
    void launchProcess(int child_stdout);

    void loadEnvironmentVariables(BSONObj env);
    void parseName(bool isMongo,
                   bool isMongodProgram,
                   bool isMongosProgram,
                   bool isMongotMockProgram,
                   const std::string& loggingPrefix,
                   const boost::filesystem::path& programName);
    void parseArgs(BSONObj args, bool isMongo, bool isMongodProgram);

    std::vector<std::string> _argv;
    std::map<std::string, std::string> _envp;
    int _port;
    int _pipe;
    ProcessId _pid;
    std::string _name;
    ProgramRegistry* _parentRegistry;
};

}  // namespace mongo::shell_utils
