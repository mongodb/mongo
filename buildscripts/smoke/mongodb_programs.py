"""
Basic utilities to start and stop mongo processes on the local machine.

Encapsulates all the nitty-gritty parameter conversion, database path setup, and custom arguments.
"""

import json
import os
import shutil
import time

from external_programs import *
from mongodb_network import *

#
# Callback functions defined for special kwargs to MongoD/MongoShell/DBTests
#


def apply_buildlogger_args(process, field, value):

    def hookup_bl(python_executable="python",
                  buildlogger_script="buildlogger.py",
                  buildlogger_global=False,
                  **kwargs):

        buildlogger_arguments = [buildlogger_script]
        if buildlogger_global:
            buildlogger_arguments.append("-g")

        buildlogger_arguments.append(process.executable)
        process.executable = python_executable

        process.arguments = buildlogger_arguments + process.arguments

        for field in kwargs:
            process.env_vars[field.upper()] = kwargs[field]

    hookup_bl(**value)

# The "buildlogger" argument is a special command-line parameter, does crazy stuff
BUILDLOGGER_CUSTOM_KWARGS = \
    {"buildlogger": (None, KWARG_TYPE_CALLBACK, apply_buildlogger_args)}


def apply_verbose_arg(process, field, value):

    verbose_arg = "v" * value
    if verbose_arg:
        process.arguments.append("-" + verbose_arg)

# The "verbose" argument is a special command-line parameter, converts to "v"s
VERBOSE_CUSTOM_KWARGS = \
    {"verbose": (None, KWARG_TYPE_CALLBACK, apply_verbose_arg)}


def apply_setparam_args(process, field, value):

    for param_name, param_value in value.iteritems():
        process.arguments.append("--setParameter")
        process.arguments.append("%s=%s" % (param_name, json.dumps(param_value)))

# The "set_parameters" arg is a special command line parameter, converts to "field=value"
SETPARAM_CUSTOM_KWARGS = \
    {"set_parameters": (None, KWARG_TYPE_CALLBACK, apply_setparam_args)}

#
# Default MongoD options
#

MONGOD_DEFAULT_EXEC = "./mongod"

MONGOD_DEFAULT_DATA_PATH = "/data/db"

MONGOD_KWARGS = dict(
    BUILDLOGGER_CUSTOM_KWARGS.items() +
    VERBOSE_CUSTOM_KWARGS.items() +
    SETPARAM_CUSTOM_KWARGS.items())


class MongoD(ExternalProgram):

    """A locally-running MongoD process."""

    def __init__(self,
                 executable=MONGOD_DEFAULT_EXEC,
                 default_data_path=MONGOD_DEFAULT_DATA_PATH,
                 preserve_dbpath=False,
                 custom_kwargs=MONGOD_KWARGS,
                 **kwargs):

        mongod_kwargs = dict(kwargs.items())

        self.host = "localhost"

        if "port" in mongod_kwargs:
            self.unused_port = UnusedPort(mongod_kwargs["port"])
        else:
            self.unused_port = UnusedPort()
            mongod_kwargs["port"] = self.unused_port.port

        self.port = mongod_kwargs["port"]

        if "dbpath" not in mongod_kwargs:
            mongod_kwargs["dbpath"] = \
                os.path.join(default_data_path, "%s-%s" % (self.host, self.port))

        self.dbpath = mongod_kwargs["dbpath"]
        self.preserve_dbpath = preserve_dbpath

        ExternalProgram.__init__(self, executable, custom_kwargs=custom_kwargs, **mongod_kwargs)

    def _cleanup(self):
        if not self.preserve_dbpath and os.path.exists(self.dbpath):
            self.logger().info("Removing data in dbpath %s" % self.dbpath)
            shutil.rmtree(self.dbpath)

    def start(self):

        try:
            self._cleanup()

            if not os.path.exists(self.dbpath):
                self.logger().info("Creating dbpath at \"%s\"" % self.dbpath)
                os.makedirs(self.dbpath)
        except:
            self.logger().error("Failed to setup dbpath at \"%s\"" % self.dbpath, exc_info=True)
            raise

        # Slightly racy - fixing is tricky
        self.unused_port.release()
        self.unused_port = None

        ExternalProgram.start(self)

    def wait_for_client(self, timeout_secs=30.0):

        timer = Timer()
        while True:

            if self.poll() is not None:
                # MongoD exited for some reason
                raise Exception(
                    "Could not connect to MongoD server at %s:%s, process ended unexpectedly." %
                    (self.host, self.port))

            try:
                # Try to connect to the mongod with a pymongo client - 30s default socket timeout
                self.client().admin.command("ismaster")
                break

            except Exception as ex:

                if timer.elapsed_secs() > timeout_secs:
                    raise Exception(
                        "Failed to connect to MongoD server at %s:%s." %
                        (self.host, self.port), ex)
                else:
                    self.logger().info("Waiting to connect to MongoD server at %s:%s..." %
                                       (self.host, self.port))
                    time.sleep(0.5)

            self.logger().info("Connected to MongoD server at %s:%s." % (self.host, self.port))

    def client(self, **client_args):
        # Import pymongo here, only when needed
        import pymongo
        return pymongo.MongoClient(self.host, self.port, **client_args)

    def _wait_for_port(self, timeout_secs=10):
        timer = Timer()
        while True:
            try:
                self.unused_port = UnusedPort(self.port)
                break
            except Exception as ex:

                if timer.elapsed_secs() > timeout_secs:
                    raise Exception("Failed to cleanup port from MongoD server at %s:%s" %
                                    (self.host, self.port), ex)

                self.logger().info("Waiting for MongoD server at %s:%s to relinquish port..." %
                                   (self.host, self.port))
                time.sleep(0.5)

    def wait(self):
        ExternalProgram.wait(self)
        # Slightly racy - fixing is tricky
        self._wait_for_port()
        self._cleanup()

    def stop(self):
        ExternalProgram.stop(self)
        # Slightly racy - fixing is tricky
        self._wait_for_port()
        self._cleanup()

#
# Default MongoShell options
#

MONGOSHELL_DEFAULT_EXEC = "./mongo"
MONGOSHELL_KWARGS = dict(BUILDLOGGER_CUSTOM_KWARGS.items())


class MongoShellContext(object):

    """The context for a mongo shell execution.

    Tests using the shell can only have APIs provided by injecting them into the shell when it
    starts - generally as global variables.

    Shell options and global variables are specified using this structure.
    """

    def __init__(self):
        self.db_address = None
        self.global_context = {}


class MongoShell(ExternalProgram):

    """A locally-running MongoDB shell process.

    Makes it easy to start with custom global variables, pointed at a custom database, etc.

    """

    def __init__(self,
                 executable=MONGOSHELL_DEFAULT_EXEC,
                 shell_context=None,
                 db_address=None,
                 global_context={},
                 js_filenames=[],
                 custom_kwargs=MONGOSHELL_KWARGS,
                 **kwargs):

        ExternalProgram.__init__(self, executable, custom_kwargs=custom_kwargs, **kwargs)

        self.shell_context = shell_context
        if not shell_context:
            self.shell_context = MongoShellContext()
            self.shell_context.db_address = db_address
            self.shell_context.global_context.update(global_context)

        self.js_filenames = js_filenames

    def build_eval_context(self):

        eval_strs = []

        for variable, variable_json in self.shell_context.global_context.iteritems():
            eval_strs.append("%s=%s;" % (variable, json.dumps(variable_json)))

        return "".join(eval_strs)

    def build_process(self):

        process_context = self.context.clone()

        if self.shell_context.global_context:

            eval_context_str = self.build_eval_context()

            if "eval" in process_context.kwargs:
                process_context.kwargs["eval"] = process_context.kwargs["eval"] + ";" + \
                    eval_context_str
            else:
                process_context.kwargs["eval"] = eval_context_str

        process = ExternalProgram.build_process(self, process_context)

        if self.shell_context.db_address:
            process.arguments.append(self.shell_context.db_address)
        else:
            process.arguments.append("--nodb")

        if self.js_filenames:
            for js_filename in self.js_filenames:
                process.arguments.append(js_filename)

        return process

#
# Default DBTest options
#

DBTEST_DEFAULT_EXEC = "./dbtest"
DBTEST_KWARGS = dict(BUILDLOGGER_CUSTOM_KWARGS.items() + VERBOSE_CUSTOM_KWARGS.items())


class DBTest(ExternalProgram):

    """A locally running MongoDB dbtest process.

    Makes it easy to start with custom named dbtests.

    """

    def __init__(self,
                 executable=DBTEST_DEFAULT_EXEC,
                 dbtest_names=[],
                 custom_kwargs=DBTEST_KWARGS,
                 **kwargs):

        ExternalProgram.__init__(self, executable, custom_kwargs=custom_kwargs, **kwargs)

        self.dbtest_names = dbtest_names

    def build_process(self):

        process = ExternalProgram.build_process(self)

        for dbtest_name in self.dbtest_names:
            process.arguments.append(dbtest_name)

        return process
