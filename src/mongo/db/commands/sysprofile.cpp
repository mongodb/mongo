// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#ifdef __linux__
#include <csignal>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif  // __linux__

#include "mongo/db/commands.h"
#include "mongo/db/commands/sysprofile_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/quick_exit.h"                   // IWYU pragma: keep
#include "mongo/util/signal_handlers_synchronous.h"  // IWYU pragma: keep

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

#ifdef __linux__
enum class PerfMode : int { record = 0, counters };

void runProfiler(const std::string& profileName, PerfMode mode, const std::string& parentPid) {
    const std::string perfBinary = "/usr/bin/perf";
    const std::string perfName = perfBinary.substr(perfBinary.find_last_of('/') + 1);
    // Clear the signal mask set from mongod so that perf can handle SIGINT properly.
    clearSignalMask();
    // Redirect stdout/stderr to /dev/null of child process.
    int console = open("/dev/null", O_RDWR);
    tassert(8387203, "Error in opening /dev/null", console > 0);
    tassert(8387204, "Can't redirect perf stdout", dup2(console, STDOUT_FILENO) != -1);
    tassert(8387205, "Can't redirect perf stderr", dup2(console, STDERR_FILENO) != -1);
    switch (mode) {
        case PerfMode::record: {
            quickExit(execl(perfBinary.c_str(),
                            perfName.c_str(),
                            "record",
                            "-g",
                            "-o",
                            profileName.c_str(),
                            "-p",
                            parentPid.c_str(),
                            nullptr));
        } break;
        case PerfMode::counters: {
            quickExit(execl(perfBinary.c_str(),
                            perfName.c_str(),
                            "stat",
                            "-e",
                            "cycles,cache-misses,cache-references,L1-dcache-loads,L1-"
                            "dcache-load-misses,"
                            "context-switches,cpu-migrations,page-faults,branch-"
                            "instructions,branch-misses,"
                            "dTLB-load-misses,dTLB-loads",
                            "-o",
                            profileName.c_str(),
                            "-p",
                            parentPid.c_str(),
                            nullptr));
        } break;
    }
}

pid_t spawn(std::string_view filename, PerfMode mode) {
    std::string pidString = std::to_string(getpid());
    std::string profileName =
        fmt::format("{}.{}", filename, mode == PerfMode::record ? "data" : "txt");
    pid_t pid = fork();
    switch (pid) {
        case -1:
            LOGV2_ERROR(8387201, "Failed to fork.", "errno"_attr = errno);
            break;
        case 0:
            // Child process.
            runProfiler(profileName, mode, pidString);
            break;
        default:
            LOGV2(8387202, "A child process is forked for perf.", "pid"_attr = pid);
            break;
    }
    return pid;
}

bool stop(pid_t pid) {
    LOGV2(8387206, "Sending a SIGINT signal to child process.", "pid"_attr = pid);
    return kill(pid, SIGINT) == 0 && waitpid(pid, nullptr, 0) == pid;
}
#endif  // __linux__

class SysProfile : public TypedCommand<SysProfile> {
    using InvocationBase = typename TypedCommand<SysProfile>::InvocationBase;

public:
    using Request = SysProfileCommandRequest;
    using Reply = SysProfileCommandRequest::Reply;

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Internal profiling command, for testing only. See "
               "https://wiki.corp.mongodb.com/display/~zixuan.zhuang@mongodb.com/"
               "Scripting+Profiler for prerequisite and examples.";
    }

    class Invocation : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {}

        Reply typedRun(OperationContext* opCtx) {
#ifdef __linux__
            LOGV2(8387208, "Test-only command 'sysprofile' invoked");
            Reply reply(1.0);
            if (auto pid = request().getPid()) {
                // kill profiler
                reply.setOk(stop(*pid));
            } else {
                std::string_view filename = request().getFilename();
                PerfMode mode = request().getMode() == ProfileModeEnum::record ? PerfMode::record
                                                                               : PerfMode::counters;
                reply.setPid(spawn(filename, mode));
            }
            return reply;
#else
            uasserted(8387207, "Unsupported OS for sysprofile command");
#endif  // __linux__
        }
    };
};

MONGO_REGISTER_COMMAND(SysProfile).testOnly().forShard();
}  // namespace
}  // namespace mongo
