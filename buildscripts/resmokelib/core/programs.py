"""
Utility functions to create MongoDB processes.

Handles all the nitty-gritty parameter conversion.
"""

from __future__ import absolute_import

import json
import os
import os.path
import stat

from . import process as _process
from .. import utils
from .. import config


def mongod_program(logger, executable=None, process_kwargs=None, **kwargs):
    """
    Returns a Process instance that starts a mongod executable with
    arguments constructed from 'kwargs'.
    """

    executable = utils.default_if_none(executable, config.DEFAULT_MONGOD_EXECUTABLE)
    args = [executable]

    # Apply the --setParameter command line argument. Command line options to resmoke.py override
    # the YAML configuration.
    suite_set_parameters = kwargs.pop("set_parameters", {})

    if config.MONGOD_SET_PARAMETERS is not None:
        suite_set_parameters.update(utils.load_yaml(config.MONGOD_SET_PARAMETERS))

    _apply_set_parameters(args, suite_set_parameters)

    shortcut_opts = {
        "nojournal": config.NO_JOURNAL,
        "nopreallocj": config.NO_PREALLOC_JOURNAL,
        "storageEngine": config.STORAGE_ENGINE,
        "wiredTigerCollectionConfigString": config.WT_COLL_CONFIG,
        "wiredTigerEngineConfigString": config.WT_ENGINE_CONFIG,
        "wiredTigerIndexConfigString": config.WT_INDEX_CONFIG,
    }

    if config.STORAGE_ENGINE == "rocksdb":
        shortcut_opts["rocksdbCacheSizeGB"] = config.STORAGE_ENGINE_CACHE_SIZE
    elif config.STORAGE_ENGINE == "wiredTiger" or config.STORAGE_ENGINE is None:
        shortcut_opts["wiredTigerCacheSizeGB"] = config.STORAGE_ENGINE_CACHE_SIZE

    # These options are just flags, so they should not take a value.
    opts_without_vals = ("nojournal", "nopreallocj")

    # Have the --nojournal command line argument to resmoke.py unset the journal option.
    if shortcut_opts["nojournal"] and "journal" in kwargs:
        del kwargs["journal"]

    # Ensure that config servers run with journaling enabled.
    if "configsvr" in kwargs:
        shortcut_opts["nojournal"] = False
        kwargs["journal"] = ""

    # Command line options override the YAML configuration.
    for opt_name in shortcut_opts:
        opt_value = shortcut_opts[opt_name]
        if opt_name in opts_without_vals:
            # Options that are specified as --flag on the command line are represented by a boolean
            # value where True indicates that the flag should be included in 'kwargs'.
            if opt_value:
                kwargs[opt_name] = ""
        else:
            # Options that are specified as --key=value on the command line are represented by a
            # value where None indicates that the key-value pair shouldn't be included in 'kwargs'.
            if opt_value is not None:
                kwargs[opt_name] = opt_value

    # Override the storage engine specified on the command line with "wiredTiger" if running a
    # config server replica set.
    if "replSet" in kwargs and "configsvr" in kwargs:
        kwargs["storageEngine"] = "wiredTiger"

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, kwargs)

    _set_keyfile_permissions(kwargs)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return _process.Process(logger, args, **process_kwargs)


def mongos_program(logger, executable=None, process_kwargs=None, **kwargs):
    """
    Returns a Process instance that starts a mongos executable with
    arguments constructed from 'kwargs'.
    """

    executable = utils.default_if_none(executable, config.DEFAULT_MONGOS_EXECUTABLE)
    args = [executable]

    # Apply the --setParameter command line argument. Command line options to resmoke.py override
    # the YAML configuration.
    suite_set_parameters = kwargs.pop("set_parameters", {})

    if config.MONGOS_SET_PARAMETERS is not None:
        suite_set_parameters.update(utils.load_yaml(config.MONGOS_SET_PARAMETERS))

    _apply_set_parameters(args, suite_set_parameters)

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, kwargs)

    _set_keyfile_permissions(kwargs)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return _process.Process(logger, args, **process_kwargs)


def mongo_shell_program(logger, executable=None, filename=None, process_kwargs=None,
                        isMainTest=True, **kwargs):
    """
    Returns a Process instance that starts a mongo shell with arguments
    constructed from 'kwargs'.
    """

    executable = utils.default_if_none(executable, config.DEFAULT_MONGO_EXECUTABLE)
    args = [executable]

    eval_sb = []  # String builder.
    global_vars = kwargs.pop("global_vars", {}).copy()

    shortcut_opts = {
        "noJournal": (config.NO_JOURNAL, False),
        "noJournalPrealloc": (config.NO_PREALLOC_JOURNAL, False),
        "storageEngine": (config.STORAGE_ENGINE, ""),
        "storageEngineCacheSizeGB": (config.STORAGE_ENGINE_CACHE_SIZE, ""),
        "testName": (os.path.splitext(os.path.basename(filename))[0], ""),
        "wiredTigerCollectionConfigString": (config.WT_COLL_CONFIG, ""),
        "wiredTigerEngineConfigString": (config.WT_ENGINE_CONFIG, ""),
        "wiredTigerIndexConfigString": (config.WT_INDEX_CONFIG, ""),
    }

    test_data = global_vars.get("TestData", {}).copy()
    for opt_name in shortcut_opts:
        (opt_value, opt_default) = shortcut_opts[opt_name]
        if opt_value is not None:
            test_data[opt_name] = opt_value
        elif opt_name not in test_data:
            # Only use 'opt_default' if the property wasn't set in the YAML configuration.
            test_data[opt_name] = opt_default

    test_data["isMainTest"] = isMainTest
    global_vars["TestData"] = test_data

    # Pass setParameters for mongos and mongod through TestData. The setParameter parsing in
    # servers.js is very primitive (just splits on commas), so this may break for non-scalar
    # setParameter values.
    if config.MONGOD_SET_PARAMETERS is not None:
        if "setParameters" in test_data:
            raise ValueError("setParameters passed via TestData can only be set from either the"
                             " command line or the suite YAML, not both")
        mongod_set_parameters = utils.load_yaml(config.MONGOD_SET_PARAMETERS)
        test_data["setParameters"] = _format_test_data_set_parameters(mongod_set_parameters)

    if config.MONGOS_SET_PARAMETERS is not None:
        if "setParametersMongos" in test_data:
            raise ValueError("setParametersMongos passed via TestData can only be set from either"
                             " the command line or the suite YAML, not both")
        mongos_set_parameters = utils.load_yaml(config.MONGOS_SET_PARAMETERS)
        test_data["setParametersMongos"] = _format_test_data_set_parameters(mongos_set_parameters)

    for var_name in global_vars:
        _format_shell_vars(eval_sb, var_name, global_vars[var_name])

    if "eval" in kwargs:
        eval_sb.append(str(kwargs.pop("eval")))

    eval_str = "; ".join(eval_sb)
    args.append("--eval")
    args.append(eval_str)

    if config.SHELL_READ_MODE is not None:
        kwargs["readMode"] = config.SHELL_READ_MODE

    if config.SHELL_WRITE_MODE is not None:
        kwargs["writeMode"] = config.SHELL_WRITE_MODE

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, kwargs)

    # Have the mongos shell run the specified file.
    args.append(filename)

    _set_keyfile_permissions(test_data)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return _process.Process(logger, args, **process_kwargs)


def _format_shell_vars(sb, path, value):
    """
    Formats 'value' in a way that can be passed to --eval.

    If 'value' is a dictionary, then it is unrolled into the creation of
    a new JSON object with properties assigned for each key of the
    dictionary.
    """

    # Only need to do special handling for JSON objects.
    if not isinstance(value, dict):
        sb.append("%s = %s" % (path, json.dumps(value)))
        return

    # Avoid including curly braces and colons in output so that the command invocation can be
    # copied and run through bash.
    sb.append("%s = new Object()" % (path))
    for subkey in value:
        _format_shell_vars(sb, ".".join((path, subkey)), value[subkey])


def dbtest_program(logger, executable=None, suites=None, process_kwargs=None, **kwargs):
    """
    Returns a Process instance that starts a dbtest executable with
    arguments constructed from 'kwargs'.
    """

    executable = utils.default_if_none(executable, config.DEFAULT_DBTEST_EXECUTABLE)
    args = [executable]

    if suites is not None:
        args.extend(suites)

    if config.STORAGE_ENGINE is not None:
        kwargs["storageEngine"] = config.STORAGE_ENGINE

    return generic_program(logger, args, process_kwargs=process_kwargs, **kwargs)

def generic_program(logger, args, process_kwargs=None, **kwargs):
    """
    Returns a Process instance that starts an arbitrary executable with
    arguments constructed from 'kwargs'. The args parameter is an array
    of strings containing the command to execute.
    """

    if not utils.is_string_list(args):
        raise ValueError("The args parameter must be a list of command arguments")

    _apply_kwargs(args, kwargs)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return _process.Process(logger, args, **process_kwargs)


def _format_test_data_set_parameters(set_parameters):
    """
    Converts key-value pairs from 'set_parameters' into the comma
    delimited list format expected by the parser in servers.js.

    WARNING: the parsing logic in servers.js is very primitive.
    Non-scalar options such as logComponentVerbosity will not work
    correctly.
    """
    params = []
    for param_name in set_parameters:
        param_value = set_parameters[param_name]
        if isinstance(param_value, bool):
            # Boolean valued setParameters are specified as lowercase strings.
            param_value = "true" if param_value else "false"
        elif isinstance(param_value, dict):
            raise TypeError("Non-scalar setParameter values are not currently supported.")
        params.append("%s=%s" % (param_name, param_value))
    return ",".join(params)

def _apply_set_parameters(args, set_parameter):
    """
    Converts key-value pairs from 'kwargs' into --setParameter key=value
    arguments to an executable and appends them to 'args'.
    """

    for param_name in set_parameter:
        param_value = set_parameter[param_name]
        # --setParameter takes boolean values as lowercase strings.
        if isinstance(param_value, bool):
            param_value = "true" if param_value else "false"
        args.append("--setParameter")
        args.append("%s=%s" % (param_name, param_value))


def _apply_kwargs(args, kwargs):
    """
    Converts key-value pairs from 'kwargs' into --key value arguments
    to an executable and appends them to 'args'.

    A --flag without a value is represented with the empty string.
    """

    for arg_name in kwargs:
        arg_value = str(kwargs[arg_name])
        args.append("--%s" % (arg_name))
        if arg_value:
            args.append(arg_value)


def _set_keyfile_permissions(opts):
    """
    Change the permissions of keyfiles in 'opts' to 600, i.e. only the
    user can read and write the file.

    This necessary to avoid having the mongod/mongos fail to start up
    because "permissions on the keyfiles are too open".

    We can't permanently set the keyfile permissions because git is not
    aware of them.
    """
    if "keyFile" in opts:
        os.chmod(opts["keyFile"], stat.S_IRUSR | stat.S_IWUSR)
    if "encryptionKeyFile" in opts:
        os.chmod(opts["encryptionKeyFile"], stat.S_IRUSR | stat.S_IWUSR)
