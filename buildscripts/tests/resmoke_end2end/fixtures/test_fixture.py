from buildscripts.resmokelib.testing.fixtures.standalone import MongoDFixture


# just a copy of MongoDFixture for testing
class MongoDFixtureTesting(MongoDFixture):
    REGISTERED_NAME = "MongoDFixtureTesting"
