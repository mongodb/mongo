/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include "s3_connection.h"
#include "s3_cleanup.h"
#include <fstream>
#include <random>

// Default config settings for the Test environment.
namespace TestDefaults {
const Aws::String region = Aws::Region::AP_SOUTHEAST_2;
const double throughputTargetGbps = 5;
const uint64_t partSize = 8 * 1024 * 1024;  // 8 MB.
static std::string bucketName("s3testext"); // Can be overridden with environment variables.

/*
 * Objects with the prefex pattern "s3test/*" are deleted after a certain period of time according
 * to the lifecycle rule on the S3 bucket. Should you wish to make any changes to the prefix pattern
 * or lifecycle of the object, please speak to the release manager.
 */
static std::string objPrefix("s3test/unit/"); // To be concatenated with a random string.
} // namespace TestDefaults

// Concatenates a random suffix to the prefix being used for the test object keys. Example of
// generated test prefix: "s3test/unit/2022-31-01-16-34-10/623843294--".
static int
randomizeTestPrefix()
{
    char timeStr[100];
    std::time_t t = std::time(nullptr);

    REQUIRE(std::strftime(timeStr, sizeof(timeStr), "%F-%H-%M-%S", std::localtime(&t)) != 0);

    TestDefaults::objPrefix += timeStr;

    // Create a random device and use it to generate a random seed to initialize the generator.
    std::random_device myRandomDevice;
    unsigned seed = myRandomDevice();
    std::default_random_engine myRandomEngine(seed);

    TestDefaults::objPrefix += '/' + std::to_string(myRandomEngine());
    TestDefaults::objPrefix += "--";

    return (0);
}

// Overrides the defaults with the ones specific for this test instance.
static int
setupTestDefaults()
{
    // Prefer to use the bucket provided through the environment variable.
    const char *envBucket = std::getenv("WT_S3_EXT_BUCKET");
    if (envBucket != nullptr)
        TestDefaults::bucketName = envBucket;
    std::cerr << "Bucket to be used for testing: " << TestDefaults::bucketName << std::endl;

    // Append the prefix to be used for object names by a unique string.
    REQUIRE(randomizeTestPrefix() == 0);
    std::cerr << "Generated prefix: " << TestDefaults::objPrefix << std::endl;

    return (0);
}

TEST_CASE("Testing class S3Connection", "s3-connection")
{
    // Setup the test environment.
    REQUIRE(setupTestDefaults() == 0);

    // Set up the config to use the defaults specified.
    Aws::S3Crt::ClientConfiguration awsConfig;
    awsConfig.region = TestDefaults::region;
    awsConfig.throughputTargetGbps = TestDefaults::throughputTargetGbps;
    awsConfig.partSize = TestDefaults::partSize;

    Aws::Auth::AWSCredentials credentials(
      std::getenv("aws_sdk_s3_ext_access_key"), std::getenv("aws_sdk_s3_ext_secret_key"));

    // Initialize the API.
    S3Connection conn(credentials, awsConfig, TestDefaults::bucketName, TestDefaults::objPrefix);
    bool exists = false;
    size_t objectSize;

    const std::string objectName = "test_object";
    const std::string fileName = "test_object.txt";
    const std::string path = "./" + fileName;

    std::ofstream File(fileName);
    std::string payload = "Test payload";
    File << payload;
    File.close();

    SECTION("Check if object exists in S3, and if object size is correct)", "[s3-connection]")
    {
        REQUIRE(conn.ObjectExists(objectName, exists, objectSize) == 0);
        REQUIRE(objectSize == 0);
        REQUIRE(conn.PutObject(objectName, fileName) == 0);
        REQUIRE(conn.ObjectExists(objectName, exists, objectSize) == 0);
        REQUIRE(exists);
        REQUIRE(objectSize == payload.length());
        REQUIRE(conn.DeleteObject(objectName) == 0);
    }

    SECTION("Gets an object from an S3 Bucket", "[s3-connection]")
    {
        REQUIRE(conn.PutObject(objectName, fileName) == 0);
        REQUIRE(std::remove(path.c_str()) == 0);        // Delete the local copy of the file.
        REQUIRE(conn.GetObject(objectName, path) == 0); // Download the file from S3

        // The file should now be in the current directory.
        std::ifstream f(path);
        REQUIRE(f.good());

        // Clean up test artifacts.
        REQUIRE(std::remove(path.c_str()) == 0);
        REQUIRE(conn.DeleteObject(objectName) == 0);
    }

    SECTION("Lists S3 objects under the test bucket.", "[s3-connection]")
    {
        std::vector<std::string> objects;

        // Total objects to insert in the test.
        const int32_t totalObjects = 20;
        // Prefix for objects in this test.
        const std::string prefix = "test_list_objects_";
        const bool listSingle = true;
        // Number of objects to access per iteration of AWS.
        int32_t batchSize = 1;

        S3Cleanup s3cleanup(conn, prefix, fileName);

        // No matching objects. Object size should be 0.
        REQUIRE(conn.ListObjects(prefix, objects) == 0);
        REQUIRE(objects.size() == 0);

        // No matching objects with listSingle. Object size should be 0.
        REQUIRE(conn.ListObjects(prefix, objects, batchSize, listSingle) == 0);
        REQUIRE(objects.size() == 0);

        // Create file to prepare for test.
        REQUIRE(std::ofstream(fileName).put('.').good());

        // Put objects to prepare for test.
        for (int i = 0; i < totalObjects; i++) {
            REQUIRE(conn.PutObject(prefix + std::to_string(i) + ".txt", fileName) == 0);
            s3cleanup.setTotalObjects(i + 1);
        }

        // List all objects. This is the size of totalObjects.
        REQUIRE(conn.ListObjects(prefix, objects) == 0);
        REQUIRE(objects.size() == totalObjects);

        // List single. Object size should be 1.
        objects.clear();
        REQUIRE(conn.ListObjects(prefix, objects, batchSize, listSingle) == 0);
        REQUIRE(objects.size() == 1);

        // There should be 11 matches with 'test_list_objects_1' prefix pattern.
        objects.clear();
        REQUIRE(conn.ListObjects(prefix + "1", objects) == 0);
        REQUIRE(objects.size() == 11);

        // List with 10 objects per AWS request. Object size should be size of totalObjects.
        objects.clear();
        batchSize = 10;
        REQUIRE(conn.ListObjects(prefix, objects, batchSize) == 0);
        REQUIRE(objects.size() == totalObjects);

        // ListSingle with 10 objects per AWS request. Object size should be 1.
        objects.clear();
        REQUIRE(conn.ListObjects(prefix, objects, batchSize, listSingle) == 0);
        REQUIRE(objects.size() == 1);
    }

    SECTION("Checks if connection to a non-existing bucket fails gracefully.", "[s3-connection]")
    {
        // Checks if connection fails with an invalid bucket name.
        REQUIRE_THROWS_WITH(S3Connection(awsConfig, "BadBucket", TestDefaults::objPrefix),
          "BadBucket : No such bucket.");

        // Checks if connection fails with dynamic allocation and an invalid bucket name.
        std::unique_ptr<S3Connection> conn2;
        REQUIRE_THROWS_WITH(
          conn2 = std::make_unique<S3Connection>(awsConfig, "BadBucket2", TestDefaults::objPrefix),
          "BadBucket2 : No such bucket.");
    }
}

int
main(int argc, char **argv)
{
    // Set the SDK options
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    int ret = Catch::Session().run(argc, argv);
    Aws::ShutdownAPI(options);

    return ret;
}
