// @file cpuprofile.cpp

/**
 * This module provides commands for starting and stopping the Google perftools
 * cpu profiler linked into mongod.
 *
 * The following command enables the not-currently-enabled profiler, and writes
 * the profile data to the specified "profileFilename."
 *     { _cpuProfilerStart: { profileFilename: '/path/on/mongod-host.prof' } }
 *
 * The following command disables the already-enabled profiler:
 *     { _cpuProfilerStop: 1}
 *
 * The commands defined here, and profiling, are only available when enabled at
 * build-time with the "--use-cpu-profiler" argument to scons.
 *
 * Example SCons command line, assuming you've installed google-perftools in
 * /usr/local:
 *
 *     scons --release --use-cpu-profiler \
 *         --cpppath=/usr/local/include --libpath=/usr/loca/lib
 */

#include "google/profiler.h"
#include "mongo/db/commands.h"

namespace mongo {

    namespace {

        /**
         * Common code for the implementation of cpu profiler commands.
         */
        class CpuProfilerCommand : public Command {
        public:
            CpuProfilerCommand( char const *name ) : Command( name ) {}
            virtual bool slaveOk() const { return true; }
            virtual bool adminOnly() const { return true; }
            virtual bool localHostOnlyIfNoAuth() const { return true; }

            // This is an abuse of the global dbmutex.  We only really need to
            // ensure that only one cpuprofiler command runs at once; it would
            // be fine for it to run concurrently with other operations.
            virtual LockType locktype() const { return WRITE; }
        };

        /**
         * Class providing implementation of the _cpuProfilerStart command.
         */
        class CpuProfilerStartCommand : public CpuProfilerCommand {
        public:
            CpuProfilerStartCommand() : CpuProfilerCommand( commandName ) {}

            virtual bool run( string const &db,
                              BSONObj &cmdObj,
                              int options,
                              string &errmsg,
                              BSONObjBuilder &result,
                              bool fromRepl );

            static char const *const commandName;
        } cpuProfilerStartCommandInstance;

        /**
         * Class providing implementation of the _cpuProfilerStop command.
         */
        class CpuProfilerStopCommand : public CpuProfilerCommand {
        public:
            CpuProfilerStopCommand() : CpuProfilerCommand( commandName ) {}

            virtual bool run( string const &db,
                              BSONObj &cmdObj,
                              int options,
                              string &errmsg,
                              BSONObjBuilder &result,
                              bool fromRepl );

            static char const *const commandName;
        } cpuProfilerStopCommandInstance;

        char const *const CpuProfilerStartCommand::commandName = "_cpuProfilerStart";
        char const *const CpuProfilerStopCommand::commandName = "_cpuProfilerStop";

        bool CpuProfilerStartCommand::run( string const &db,
                                           BSONObj &cmdObj,
                                           int options,
                                           string &errmsg,
                                           BSONObjBuilder &result,
                                           bool fromRepl ) {

            std::string profileFilename = cmdObj[commandName]["profileFilename"].String();
            if ( ! ::ProfilerStart( profileFilename.c_str() ) ) {
                errmsg = "Failed to start profiler";
                return false;
            }
            return true;
        }

        bool CpuProfilerStopCommand::run( string const &db,
                                          BSONObj &cmdObj,
                                          int options,
                                          string &errmsg,
                                          BSONObjBuilder &result,
                                          bool fromRepl ) {
            ::ProfilerStop();
            return true;
        }

    }  // namespace

}  // namespace mongo

