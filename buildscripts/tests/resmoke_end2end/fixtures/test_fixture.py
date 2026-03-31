from buildscripts.resmokelib.testing.fixtures.standalone import MongoDFixture


# Test fixture for external module loading
class MongoDFixtureTesting(MongoDFixture):
    REGISTERED_NAME = "MongoDFixtureTesting"
