"""Utility for making resmoke suites external compatible."""

# The `make_external` util function in this file makes a suite
# compatible to run with an External System Under Test (SUT).
# For more info on External SUT, look into the `--externalSUT`
# flag in `resmoke.py run --help`. Currently, External SUT
# testing is only used in Antithesis, so some of these changes
# are specific to Antithesis testing with External SUTs

from buildscripts.resmokelib import logging
from buildscripts.resmokelib.utils.dictionary import set_dict_value

INCOMPATIBLE_HOOKS = [
    "CleanEveryN",
    "ContinuousStepdown",
    "CheckOrphansDeleted",
]


def delete_archival(suite):
    """Remove archival for External Suites."""
    logging.loggers.ROOT_EXECUTOR_LOGGER.warning(
        "`archive` is not supported for external suites and will be removed if it exists."
    )
    suite.pop("archive", None)
    suite.get("executor", {}).pop("archive", None)


def make_hooks_compatible(suite):
    """Make hooks compatible for external suites."""
    logging.loggers.ROOT_EXECUTOR_LOGGER.warning(
        "Some hooks are automatically disabled for external suites: %s", INCOMPATIBLE_HOOKS
    )
    logging.loggers.ROOT_EXECUTOR_LOGGER.warning(
        "The `AntithesisLogging` hook is automatically added for external suites."
    )
    if suite.get("executor", {}).get("hooks", None):
        # it's a list of dicts, each with key 'class'
        converted_hooks = [{"class": "AntithesisLogging"}]
        for hook in suite["executor"]["hooks"]:
            assert isinstance(
                hook, dict
            ), f"Unknown structure in hook. Please reach out in #server-testing: {hook}"

            name = hook["class"]
            if name in INCOMPATIBLE_HOOKS:
                continue

            if name in ["ValidateCollections", "BackgroundInitialSync", "IntermediateInitialSync"]:
                # Fast count can be incorrect in case of an unclean shutdown, which may occur in Antithesis.
                set_dict_value(
                    hook,
                    ["shell_options", "global_vars", "TestData", "skipEnforceFastCountOnValidate"],
                    True,
                )

            converted_hooks.append(hook)

        suite["executor"]["hooks"] = converted_hooks


def update_test_data(suite):
    """Update TestData to be compatible with external suites."""
    logging.loggers.ROOT_EXECUTOR_LOGGER.warning(
        "`useActionPermittedFile` is incompatible with external suites and will always be set to `False`."
    )
    suite.setdefault("executor", {}).setdefault("config", {}).setdefault(
        "shell_options", {}
    ).setdefault("global_vars", {}).setdefault("TestData", {}).update(
        {"useActionPermittedFile": False}
    )


def update_shell(suite):
    """Update shell for when running external suites."""
    logging.loggers.ROOT_EXECUTOR_LOGGER.warning(
        "`jsTestLog` is a no-op on external suites to reduce logging."
    )
    suite.setdefault("executor", {}).setdefault("config", {}).setdefault(
        "shell_options", {}
    ).setdefault("eval", "")
    suite["executor"]["config"]["shell_options"]["eval"] += "jsTestLog = Function.prototype;"


def update_exclude_tags(suite):
    """Update the exclude tags to exclude external suite incompatible tests."""
    logging.loggers.ROOT_EXECUTOR_LOGGER.warning(
        "The `antithesis_incompatible` tagged tests will be excluded for external suites."
    )
    suite.setdefault("selector", {})
    if not suite.get("selector").get("exclude_with_any_tags"):
        suite["selector"]["exclude_with_any_tags"] = ["antithesis_incompatible"]
    else:
        suite["selector"]["exclude_with_any_tags"].append("antithesis_incompatible")


def make_external(suite):
    """Modify suite in-place to be external compatible."""
    logging.loggers.ROOT_EXECUTOR_LOGGER.warning(
        "This suite is being converted to an 'External Suite': %s", suite
    )
    delete_archival(suite)
    make_hooks_compatible(suite)
    update_test_data(suite)
    update_shell(suite)
    update_exclude_tags(suite)

    return suite
