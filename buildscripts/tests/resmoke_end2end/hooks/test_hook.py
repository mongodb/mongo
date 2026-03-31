from buildscripts.resmokelib.testing.hooks.validate import ValidateCollections


# Test hook for external module loading
class ValidateCollectionsTesting(ValidateCollections):
    REGISTERED_NAME = "ValidateCollectionsTesting"
