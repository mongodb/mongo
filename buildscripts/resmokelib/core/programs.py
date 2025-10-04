"""Utility functions for creating MongoDB processes.

Handles all the nitty-gritty parameter conversion.
"""

import json
import os
import os.path
import re
import stat
from typing import Any, Optional, Tuple

from packaging import version

from buildscripts.resmokelib import config, logging, utils
from buildscripts.resmokelib.core import network, process
from buildscripts.resmokelib.testing.fixtures import shardedcluster, standalone
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib
from buildscripts.resmokelib.utils.history import HistoryDict, make_historic


def make_process(*args, **kwargs) -> process.Process:
    """Set up the environment for subprocesses."""
    process_cls = process.Process

    # Add the current working directory and /data/multiversion to the PATH.
    env_vars = kwargs.get("env_vars", {}).copy()
    path = get_path_env_var(env_vars)

    if config.INSTALL_DIR is not None:
        env_vars["INSTALL_DIR"] = config.INSTALL_DIR

    env_vars["PATH"] = os.pathsep.join(path)
    kwargs["env_vars"] = env_vars
    return process_cls(*args, **kwargs)


def get_path_env_var(env_vars):
    """Return the path base on provided environment variable."""
    path = [os.getcwd()] + config.MULTIVERSION_DIRS
    # If installDir is provided, add it early to the path
    if config.INSTALL_DIR is not None:
        path.append(config.INSTALL_DIR)
    path.append(env_vars.get("PATH", os.environ.get("PATH", "")))
    return path


def get_binary_version(executable):
    """Return the string for the binary version of the given executable."""

    from buildscripts.resmokelib.multiversionconstants import LATEST_FCV

    split_executable = os.path.basename(executable).split("-")
    version_regex = re.compile(version.VERSION_PATTERN, re.VERBOSE | re.IGNORECASE)
    if len(split_executable) > 1 and version_regex.match(split_executable[-1]):
        return split_executable[-1]
    return LATEST_FCV


def remove_set_parameter_if_before_version(
    set_parameters, parameter_name, bin_version, required_bin_version
):
    """
    Used for removing a server parameter that does not exist prior to a specified version.

    Remove 'parameter_name' from the 'set_parameters' dictionary if 'bin_version' is older than
    'required_bin_version'.
    """
    if version.parse(bin_version) < version.parse(required_bin_version):
        set_parameters.pop(parameter_name, None)


def mongod_program(
    logger: logging.Logger,
    job_num: int,
    executable: str,
    process_kwargs: dict,
    mongod_options: HistoryDict,
) -> tuple[process.Process, HistoryDict]:
    """
    Return a Process instance that starts mongod arguments constructed from 'mongod_options'.

    @param logger - The logger to pass into the process.
    @param job_num - The Resmoke job number running this process.
    @param executable - The mongod executable to run.
    @param process_kwargs - A dict of key-value pairs to pass to the process.
    @param mongod_options - A HistoryDict describing the various options to pass to the mongod.
    """

    bin_version = get_binary_version(executable)
    args = [executable]
    mongod_options = mongod_options.copy()

    if config.NOOP_MONGO_D_S_PROCESSES:
        args[0] = os.path.basename(args[0])
        mongod_options["set_parameters"]["fassertOnLockTimeoutForStepUpDown"] = 0
        mongod_options["set_parameters"].pop("backtraceLogFile", None)
        mongod_options.update(
            {
                "logpath": "/var/log/mongodb/mongodb.log",
                "dbpath": "/data/db",
                "bind_ip": "0.0.0.0",
                "oplogSize": "256",
                "wiredTigerCacheSizeGB": "1",
            }
        )

    if config.TLS_MODE:
        mongod_options["tlsMode"] = config.TLS_MODE
        if config.TLS_MODE != "disabled":
            # Note: "tlsAllowInvalidCertificates" is enabled to avoid
            # hostname conflicts with our testing certificates.
            # The ssl and ssl_special suites handle hostname validation testing.
            mongod_options["tlsAllowInvalidHostnames"] = ""

    if config.MONGOD_TLS_CERTIFICATE_KEY_FILE:
        mongod_options["tlsCertificateKeyFile"] = config.MONGOD_TLS_CERTIFICATE_KEY_FILE

    if config.TLS_CA_FILE:
        mongod_options["tlsCAFile"] = config.TLS_CA_FILE

    if "port" not in mongod_options:
        mongod_options["port"] = network.PortAllocator.next_fixture_port(job_num)

    suite_set_parameters = mongod_options.get("set_parameters", {})
    remove_set_parameter_if_before_version(
        suite_set_parameters, "queryAnalysisSamplerConfigurationRefreshSecs", bin_version, "7.0.0"
    )
    remove_set_parameter_if_before_version(
        suite_set_parameters, "queryAnalysisWriterIntervalSecs", bin_version, "7.0.0"
    )
    remove_set_parameter_if_before_version(
        suite_set_parameters, "defaultConfigCommandTimeoutMS", bin_version, "7.3.0"
    )

    remove_set_parameter_if_before_version(
        suite_set_parameters, "internalQueryStatsRateLimit", bin_version, "7.3.0"
    )
    remove_set_parameter_if_before_version(
        suite_set_parameters, "internalQueryStatsErrorsAreCommandFatal", bin_version, "7.3.0"
    )
    remove_set_parameter_if_before_version(
        suite_set_parameters, "enableAutoCompaction", bin_version, "7.3.0"
    )
    remove_set_parameter_if_before_version(
        suite_set_parameters, "findShardsOnConfigTimeoutMS", bin_version, "8.3.0"
    )

    if "grpcPort" not in mongod_options and suite_set_parameters.get("featureFlagGRPC"):
        mongod_options["grpcPort"] = network.PortAllocator.next_fixture_port(job_num)

    _apply_set_parameters(args, suite_set_parameters)
    final_mongod_options = mongod_options.copy()
    mongod_options.pop("set_parameters")

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, mongod_options)

    _set_keyfile_permissions(mongod_options)

    process_kwargs = make_historic(utils.default_if_none(process_kwargs, {}))
    if config.EXPORT_MONGOD_CONFIG == "regular":
        mongod_options.dump_history(f"{logger.name}_config.yml")
    elif config.EXPORT_MONGOD_CONFIG == "detailed":
        mongod_options.dump_history(f"{logger.name}_config.yml", include_location=True)

    return make_process(logger, args, **process_kwargs), final_mongod_options


def mongos_program(
    logger: logging.Logger,
    job_num: int,
    executable: Optional[str] = None,
    process_kwargs: Optional[dict] = None,
    mongos_options: dict = None,
) -> Tuple[process.Process, dict]:
    """Return a Process instance that starts a mongos with arguments constructed from 'kwargs'."""
    bin_version = get_binary_version(executable)
    args = [executable]

    mongos_options = mongos_options.copy()
    mongos_options.setdefault("set_parameters", {})

    if config.NOOP_MONGO_D_S_PROCESSES:
        args[0] = os.path.basename(args[0])
        mongos_options["set_parameters"]["fassertOnLockTimeoutForStepUpDown"] = 0
        mongos_options.update({"logpath": "/var/log/mongodb/mongodb.log", "bind_ip": "0.0.0.0"})

    if config.TLS_MODE:
        mongos_options["tlsMode"] = config.TLS_MODE
        if config.TLS_MODE != "disabled":
            mongos_options["tlsAllowInvalidHostnames"] = ""

    if config.MONGOS_TLS_CERTIFICATE_KEY_FILE:
        mongos_options["tlsCertificateKeyFile"] = config.MONGOS_TLS_CERTIFICATE_KEY_FILE

    if config.TLS_CA_FILE:
        mongos_options["tlsCAFile"] = config.TLS_CA_FILE

    if "port" not in mongos_options:
        mongos_options["port"] = network.PortAllocator.next_fixture_port(job_num)

    suite_set_parameters = mongos_options.get("set_parameters", {})
    remove_set_parameter_if_before_version(
        suite_set_parameters, "queryAnalysisSamplerConfigurationRefreshSecs", bin_version, "7.0.0"
    )
    remove_set_parameter_if_before_version(
        suite_set_parameters, "defaultConfigCommandTimeoutMS", bin_version, "7.3.0"
    )

    remove_set_parameter_if_before_version(
        suite_set_parameters, "internalQueryStatsRateLimit", bin_version, "7.3.0"
    )
    remove_set_parameter_if_before_version(
        suite_set_parameters, "internalQueryStatsErrorsAreCommandFatal", bin_version, "7.3.0"
    )
    remove_set_parameter_if_before_version(
        suite_set_parameters, "findShardsOnConfigTimeoutMS", bin_version, "8.3.0"
    )
    remove_set_parameter_if_before_version(
        suite_set_parameters, "maxRoundsWithoutProgressParameter", bin_version, "8.2.0"
    )

    if "grpcPort" not in mongos_options and suite_set_parameters.get("featureFlagGRPC"):
        mongos_options["grpcPort"] = network.PortAllocator.next_fixture_port(job_num)

    _apply_set_parameters(args, suite_set_parameters)
    final_mongos_options = mongos_options.copy()
    mongos_options.pop("set_parameters")

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, mongos_options)

    _set_keyfile_permissions(mongos_options)

    process_kwargs = make_historic(utils.default_if_none(process_kwargs, {}))

    return make_process(logger, args, **process_kwargs), final_mongos_options


def mongot_program(
    logger, job_num, executable=None, process_kwargs=None, mongot_options=None
) -> Tuple[process.Process, Any]:
    """Return a Process instance that starts a mongot."""
    args = [executable]
    mongot_options = mongot_options.copy()
    final_mongot_options = mongot_options.copy()
    # Apply the rest of the command line arguments.
    _apply_kwargs(args, mongot_options)
    process_kwargs = make_historic(utils.default_if_none(process_kwargs, {}))
    return make_process(logger, args, **process_kwargs), final_mongot_options


def mongo_shell_program(
    logger: logging.Logger,
    test_name: str,
    executable: Optional[str] = None,
    connection_string: Optional[str] = None,
    filenames: Optional[list[str]] = None,
    process_kwargs: Optional[dict] = None,
    **kwargs,
) -> process.Process:
    """Return a Process instance that starts a mongo shell.

    Args:
        logger (logging.Logger): logger
        test_name (str): Name of the test. Passed into the test as 'testName'.
        executable (Optional[str], optional): Path to the mongo shell binary. Defaults to None. If not given will be inferred.
        connection_string (Optional[str], optional): Connection string to mongodb. Defaults to None. If not given will be inferred.
        filenames (Optional[list[str]], optional): The files to run by the mongo shell in a single invocation. Defaults to None.
        process_kwargs (Optional[dict], optional): Extra args to pass into make_process. Defaults to None.

    Returns
        process.Process: The mongo shell invocation.
    """

    executable = utils.default_if_none(
        utils.default_if_none(executable, config.MONGO_EXECUTABLE), config.DEFAULT_MONGO_EXECUTABLE
    )
    args = [executable]

    eval_sb = []  # String builder.
    global_vars = kwargs.pop("global_vars", {}).copy()

    # the Shell fixtures uses hyphen-delimited versions (e.g. last-lts) while resmoke.py
    # uses underscore (e.g. last_lts). resmoke's version is needed as it's part of the task name.
    shell_mixed_version = (config.MULTIVERSION_BIN_VERSION or "").replace("_", "-")

    shortcut_opts = {
        "backupOnRestartDir": (config.BACKUP_ON_RESTART_DIR, None),
        "mixedBinVersions": (config.MIXED_BIN_VERSIONS, ""),
        "multiversionBinVersion": (shell_mixed_version, ""),
        "storageEngine": (config.STORAGE_ENGINE, ""),
        "storageEngineCacheSizeGB": (config.STORAGE_ENGINE_CACHE_SIZE, ""),
        "storageEngineCacheSizePct": (config.STORAGE_ENGINE_CACHE_SIZE_PCT, ""),
        "testName": (test_name, ""),
        "wiredTigerCollectionConfigString": (config.WT_COLL_CONFIG, ""),
        "wiredTigerEngineConfigString": (config.WT_ENGINE_CONFIG, ""),
        "wiredTigerIndexConfigString": (config.WT_INDEX_CONFIG, ""),
        "pauseAfterPopulate": (config.PAUSE_AFTER_POPULATE, None),
    }

    test_data = global_vars.get("TestData", {}).copy()
    for opt_name in shortcut_opts:
        (opt_value, opt_default) = shortcut_opts[opt_name]
        if opt_value is not None:
            test_data[opt_name] = opt_value
        elif opt_name not in test_data:
            # Only use 'opt_default' if the property wasn't set in the YAML configuration.
            test_data[opt_name] = opt_default

    if config.CONFIG_FUZZER_ENCRYPTION_OPTS:
        for opt_name in config.CONFIG_FUZZER_ENCRYPTION_OPTS:
            if opt_name in test_data:
                continue

            test_data[opt_name] = config.CONFIG_FUZZER_ENCRYPTION_OPTS[opt_name]

    if config.LOG_FORMAT:
        test_data["logFormat"] = config.LOG_FORMAT

    level_names_to_numbers = {"ERROR": 1, "WARNING": 2, "INFO": 3, "DEBUG": 4}
    # Convert Log Level from string to numbered values. Defaults to using "INFO".
    test_data["logLevel"] = level_names_to_numbers.get(config.LOG_LEVEL, 3)

    if config.SHELL_TLS_ENABLED:
        test_data["shellTlsEnabled"] = True

        if config.SHELL_TLS_CERTIFICATE_KEY_FILE:
            test_data["shellTlsCertificateKeyFile"] = config.SHELL_TLS_CERTIFICATE_KEY_FILE

    if config.SHELL_GRPC:
        test_data["shellGRPC"] = True

    if config.TLS_CA_FILE:
        test_data["tlsCAFile"] = config.TLS_CA_FILE

    if config.TLS_MODE:
        test_data["tlsMode"] = config.TLS_MODE

    if config.MONGOD_TLS_CERTIFICATE_KEY_FILE:
        test_data["mongodTlsCertificateKeyFile"] = config.MONGOD_TLS_CERTIFICATE_KEY_FILE

    if config.MONGOS_TLS_CERTIFICATE_KEY_FILE:
        test_data["mongosTlsCertificateKeyFile"] = config.MONGOS_TLS_CERTIFICATE_KEY_FILE

    global_vars["TestData"] = test_data

    if config.EVERGREEN_TASK_ID is not None:
        test_data["inEvergreen"] = True
        test_data["evergreenTaskId"] = config.EVERGREEN_TASK_ID
        test_data["evergreenVariantName"] = config.EVERGREEN_VARIANT_NAME

    if config.SHELL_SEED is not None:
        test_data["seed"] = int(config.SHELL_SEED)

    # Initialize setParameters for mongod and mongos, to be passed to the shell via TestData. Since
    # they are dictionaries, they will be converted to JavaScript objects when passed to the shell
    # by the _format_shell_vars() function.
    mongod_set_parameters = test_data.get("setParameters", {}).copy()
    mongos_set_parameters = test_data.get("setParametersMongos", {}).copy()
    mongocryptd_set_parameters = test_data.get("setParametersMongocryptd", {}).copy()
    mongo_set_parameters = test_data.get("setParametersMongo", {}).copy()

    feature_flag_dict = {}
    if config.ENABLED_FEATURE_FLAGS is not None:
        feature_flag_dict = {ff: "true" for ff in config.ENABLED_FEATURE_FLAGS}

    if config.DISABLED_FEATURE_FLAGS is not None:
        feature_flag_dict |= {ff: "false" for ff in config.DISABLED_FEATURE_FLAGS}

    # Propagate additional setParameters to mongod processes spawned by the mongo shell. Command
    # line options to resmoke.py override the YAML configuration.
    if config.MONGOD_SET_PARAMETERS is not None:
        mongod_set_parameters.update(utils.load_yaml(config.MONGOD_SET_PARAMETERS))
        mongod_set_parameters.update(feature_flag_dict)

    # Propagate additional setParameters to mongos processes spawned by the mongo shell. Command
    # line options to resmoke.py override the YAML configuration.
    if config.MONGOS_SET_PARAMETERS is not None:
        mongos_set_parameters.update(utils.load_yaml(config.MONGOS_SET_PARAMETERS))
        mongos_set_parameters.update(feature_flag_dict)

    # Propagate additional setParameters to mongocryptd processes spawned by the mongo shell.
    # Command line options to resmoke.py override the YAML configuration.
    if config.MONGOCRYPTD_SET_PARAMETERS is not None:
        mongocryptd_set_parameters.update(utils.load_yaml(config.MONGOCRYPTD_SET_PARAMETERS))
        mongocryptd_set_parameters.update(feature_flag_dict)

    if config.MONGO_SET_PARAMETERS is not None:
        mongo_set_parameters.update(utils.load_yaml(config.MONGO_SET_PARAMETERS))

    fixturelib = FixtureLib()
    mongod_launcher = standalone.MongodLauncher(fixturelib)

    # If the 'logComponentVerbosity' setParameter for mongod was not already specified, we set its
    # value to a default.
    mongod_set_parameters.setdefault(
        "logComponentVerbosity", mongod_launcher.get_default_log_component_verbosity_for_mongod()
    )

    mongos_launcher = shardedcluster.MongosLauncher(fixturelib)
    # If the 'logComponentVerbosity' setParameter for mongos was not already specified, we set its
    # value to a default.
    mongos_set_parameters.setdefault(
        "logComponentVerbosity", mongos_launcher.default_mongos_log_component_verbosity()
    )

    test_data["setParameters"] = mongod_set_parameters
    test_data["setParametersMongos"] = mongos_set_parameters
    test_data["setParametersMongocryptd"] = mongocryptd_set_parameters
    test_data["setShellParameters"] = mongo_set_parameters

    if "configShard" not in test_data and config.CONFIG_SHARD is not None:
        test_data["configShard"] = True

    # There's a periodic background thread that checks for and aborts expired transactions.
    # "transactionLifetimeLimitSeconds" specifies for how long a transaction can run before expiring
    # and being aborted by the background thread. It defaults to 60 seconds, which is too short to
    # be reliable for our tests. Setting it to 24 hours, so that it is longer than the Evergreen
    # execution timeout.
    if "transactionLifetimeLimitSeconds" not in test_data:
        test_data["transactionLifetimeLimitSeconds"] = 24 * 60 * 60

    if "eval_prepend" in kwargs:
        eval_sb.append(str(kwargs.pop("eval_prepend")))

    if config.SHELL_GRPC:
        eval_sb.append('await import("jstests/libs/override_methods/enable_grpc_on_connect.js")')

    # If nodb is specified, pass the connection string through TestData so it can be used inside the
    # test, then delete it so it isn't given as an argument to the mongo shell.
    if "nodb" in kwargs and connection_string is not None:
        test_data["connectionString"] = connection_string
        connection_string = None

    if config.FUZZ_MONGOD_CONFIGS is not None and config.FUZZ_MONGOD_CONFIGS is not False:
        test_data["fuzzMongodConfigs"] = True

    for var_name in global_vars:
        _format_shell_vars(eval_sb, [var_name], global_vars[var_name])

    if "eval" in kwargs:
        eval_sb.append(str(kwargs.pop("eval")))

    # Load a callback to check that the cluster-wide metadata is consistent.
    eval_sb.append('await import("jstests/libs/override_methods/check_metadata_consistency.js")')

    # Load this file to allow a callback to validate collections before shutting down mongod.
    eval_sb.append(
        'await import("jstests/libs/override_methods/validate_collections_on_shutdown.js")'
    )

    # Load a callback to check UUID consistency before shutting down a ShardingTest.
    eval_sb.append(
        'await import("jstests/libs/override_methods/check_uuids_consistent_across_cluster.js")'
    )

    # Load a callback to check index consistency before shutting down a ShardingTest.
    eval_sb.append(
        'await import("jstests/libs/override_methods/check_indexes_consistent_across_cluster.js")'
    )

    # Load a callback to check that all orphans are deleted before shutting down a ShardingTest.
    eval_sb.append('await import("jstests/libs/override_methods/check_orphans_are_deleted.js")')

    # Load a callback to check that the info stored in config.collections and config.chunks is
    # semantically correct before shutting down a ShardingTest.
    eval_sb.append(
        'await import("jstests/libs/override_methods/check_routing_table_consistency.js")'
    )

    # Load a callback to check that all shards have correct filtering information before shutting
    # down a ShardingTest.
    eval_sb.append(
        'await import("jstests/libs/override_methods/check_shard_filtering_metadata.js")'
    )

    if config.FUZZ_MONGOD_CONFIGS is not None and config.FUZZ_MONGOD_CONFIGS is not False:
        # Prevent commands from running with the config fuzzer.
        eval_sb.append(
            'await import("jstests/libs/override_methods/config_fuzzer_incompatible_commands.js")'
        )

    # Load this file to retry operations that fail due to in-progress background operations.
    eval_sb.append(
        'await import("jstests/libs/override_methods/index_builds/implicitly_retry_on_background_op_in_progress.js")'
    )

    eval_sb.append(
        '(function() { Timestamp.prototype.toString = function() { throw new Error("Cannot toString timestamps. Consider using timestampCmp() for comparison or tojson(<variable>) for output."); } })()'
    )

    mocha_grep = json.dumps(config.MOCHA_GREP)
    eval_sb.append(f"globalThis._mocha_grep = {mocha_grep};")

    eval_str = "; ".join(eval_sb)
    args.append("--eval")
    args.append(eval_str)

    if config.SHELL_TLS_ENABLED:
        args.extend(["--tls", "--tlsAllowInvalidHostnames"])
        if config.TLS_CA_FILE:
            kwargs["tlsCAFile"] = config.TLS_CA_FILE
        if config.SHELL_TLS_CERTIFICATE_KEY_FILE:
            kwargs["tlsCertificateKeyFile"] = config.SHELL_TLS_CERTIFICATE_KEY_FILE

    # mongotmock testing with gRPC requires that the shell establish a connection with mongotmock
    # over gRPC.
    if config.SHELL_GRPC or mongod_set_parameters.get("useGrpcForSearch"):
        args.append("--gRPC")

    if connection_string is not None:
        # The --host and --port options are ignored by the mongo shell when an explicit connection
        # string is specified. We remove these options to avoid any ambiguity with what server the
        # logged mongo shell invocation will connect to.
        if "port" in kwargs:
            kwargs.pop("port")

        if "host" in kwargs:
            kwargs.pop("host")

    for key in mongo_set_parameters:
        val = str(mongo_set_parameters[key])
        args.append(f"--setShellParameter={key}={val}")

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, kwargs)

    if connection_string is not None:
        args.append(connection_string)

    # Have the mongo shell run the specified file.
    if filenames:
        args.extend(filenames)

    _set_keyfile_permissions(test_data)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return make_process(logger, args, **process_kwargs)


def _format_shell_vars(sb, paths, value):
    """
    Format 'value' in a way that can be passed to --eval.

    :param sb: string builder array for the output string.
    :param paths: path of keys represented as a list.
    :param value: value in the object corresponding to the keys in `paths`
    :return: Nothing.
    """

    # Convert the list ["a", "b", "c"] into the string 'a["b"]["c"]'
    def bracketize(lst):
        return lst[0] + "".join(f'["{i}"]' for i in lst[1:])

    # Only need to do special handling for JSON objects.
    if not isinstance(value, (dict, HistoryDict)):
        sb.append("%s = %s" % (bracketize(paths), json.dumps(value)))
        return

    # Avoid including curly braces and colons in output so that the command invocation can be
    # copied and run through bash.
    sb.append("%s = new Object()" % bracketize(paths))
    for subkey in value:
        _format_shell_vars(sb, paths + [subkey], value[subkey])


def dbtest_program(
    logger, executable=None, suites=None, process_kwargs=None, **kwargs
) -> process.Process:
    """Return a Process instance that starts a dbtest with arguments constructed from 'kwargs'."""

    executable = utils.default_if_none(executable, config.DEFAULT_DBTEST_EXECUTABLE)
    args = [executable]

    if suites is not None:
        args.extend(suites)

    if config.STORAGE_ENGINE is not None:
        kwargs["storageEngine"] = config.STORAGE_ENGINE

    return generic_program(logger, args, process_kwargs=process_kwargs, **kwargs)


def genny_program(logger, executable=None, process_kwargs=None, **kwargs) -> process.Process:
    """Return a Process instance that starts a genny executable with arguments constructed from 'kwargs'."""
    executable = utils.default_if_none(executable, config.DEFAULT_GENNY_EXECUTABLE)
    args = [executable]
    return generic_program(logger, args, process_kwargs=process_kwargs, **kwargs)


def generic_program(logger, args, process_kwargs=None, **kwargs) -> process.Process:
    """Return a Process instance that starts an arbitrary executable.

    The executable arguments are constructed from 'kwargs'.

    The args parameter is an array of strings containing the command to execute.
    """

    if not utils.is_string_list(args):
        raise ValueError("The args parameter must be a list of command arguments")

    _apply_kwargs(args, kwargs)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return make_process(logger, args, **process_kwargs)


def _apply_set_parameters(args, set_parameter):
    """Convert key-value pairs from 'kwargs' into --setParameter key=value arguments.

    This result is appended to 'args'.
    """

    for param_name in set_parameter:
        param_value = set_parameter[param_name]
        # --setParameter takes boolean values as lowercase strings.
        if isinstance(param_value, bool):
            param_value = "true" if param_value else "false"
        args.append("--setParameter")
        args.append("%s=%s" % (param_name, param_value))


def _apply_kwargs(args, kwargs):
    """Convert key-value pairs from 'kwargs' into --key value arguments.

    This result is appended to 'args'.
    A --flag without a value is represented with the empty string.
    """

    for arg_name in kwargs:
        arg_value = str(kwargs[arg_name])
        if arg_value:
            args.append("--%s=%s" % (arg_name, arg_value))
        else:
            args.append("--%s" % (arg_name))


def _set_keyfile_permissions(opts):
    """Change the permissions of keyfiles in 'opts' to 600, (only user can read and write the file).

    This necessary to avoid having the mongod/mongos fail to start up
    because "permissions on the keyfiles are too open".

    We can't permanently set the keyfile permissions because git is not
    aware of them.
    """
    for keysuffix in ["1", "2", "ForRollover"]:
        keyfile = "jstests/libs/key%s" % keysuffix
        if os.path.exists(keyfile):
            os.chmod(keyfile, stat.S_IRUSR | stat.S_IWUSR)

    if "keyFile" in opts:
        os.chmod(opts["keyFile"], stat.S_IRUSR | stat.S_IWUSR)
    if "encryptionKeyFile" in opts:
        os.chmod(opts["encryptionKeyFile"], stat.S_IRUSR | stat.S_IWUSR)
