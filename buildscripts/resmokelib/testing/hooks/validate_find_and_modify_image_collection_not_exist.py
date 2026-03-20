"""Hook that asserts config.image_collection does not exist after each test."""

import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class ValidateFindAndModifyImageCollectionNotExist(jsfile.JSHook):
    """After each test, asserts that config.image_collection does not exist.
    If it exists, the hook prints the documents in the collection and fails.
    """

    REGISTERED_NAME = "ValidateFindAndModifyImageCollectionNotExist"
    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize ValidateFindAndModifyImageCollectionNotExist."""
        description = "Assert config.image_collection does not exist after test"
        js_filename = os.path.join(
            "jstests", "hooks", "run_validate_find_and_modify_image_collection_not_exist.js"
        )
        jsfile.JSHook.__init__(
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
