#include <s3_connection.h>
#include <fstream>
#include <random>

/* Default config settings for the Test environment. */
namespace TestDefaults {
const Aws::String region = Aws::Region::AP_SOUTHEAST_2;
const double throughputTargetGbps = 5;
const uint64_t partSize = 8 * 1024 * 1024;  /* 8 MB. */
static std::string bucketName("s3testext"); // Can be overridden with environment variables.
static std::string objPrefix("s3test_artefacts/unit_"); // To be concatenated with a random string.
} // namespace TestDefaults

#define TEST_SUCCESS 0
#define TEST_FAILURE 1

int TestListObjects(const Aws::S3Crt::ClientConfiguration &config);
int TestGetObject(const Aws::S3Crt::ClientConfiguration &config);
int TestObjectExists(const Aws::S3Crt::ClientConfiguration &config);

/* Wrapper for unit test functions. */
#define TEST(func, config)                            \
    do {                                              \
        int __ret;                                    \
        if ((__ret = (func(config))) != TEST_SUCCESS) \
            return (__ret);                           \
    } while (0)

/*
 * randomizeTestPrefix --
 *     Concatenates a random suffix to the prefix being used for the test object keys. Example of
 *     generated test prefix: "s3test_artefacts/unit_" 2022-31-01-16-34-10_623843294/"
 */
static int
randomizeTestPrefix()
{
    char timeStr[100];
    std::time_t t = std::time(nullptr);

    if (std::strftime(timeStr, sizeof(timeStr), "%F-%H-%M-%S", std::localtime(&t)) == 0)
        return (TEST_FAILURE);

    TestDefaults::objPrefix += timeStr;

    /* Create a random device and use it to generate a random seed to initialize the generator. */
    std::random_device myRandomDevice;
    unsigned seed = myRandomDevice();
    std::default_random_engine myRandomEngine(seed);

    TestDefaults::objPrefix += '_' + std::to_string(myRandomEngine());
    TestDefaults::objPrefix += '/';

    return (TEST_SUCCESS);
}

/*
 * setupTestDefaults --
 *     Override the defaults with the ones specific for this test instance.
 */
static int
setupTestDefaults()
{
    /* Prefer to use the bucket provided through the environment variable. */
    const char *envBucket = std::getenv("WT_S3_EXT_BUCKET");
    if (envBucket != NULL)
        TestDefaults::bucketName = envBucket;
    std::cerr << "Bucket to be used for testing: " << TestDefaults::bucketName << std::endl;

    /* Append the prefix to be used for object names by a unique string. */
    if (randomizeTestPrefix() != 0)
        return (TEST_FAILURE);
    std::cerr << "Generated prefix: " << TestDefaults::objPrefix << std::endl;

    return (TEST_SUCCESS);
}

static int
CleanupTestListObjects(S3Connection &conn, const int totalObjects, const std::string &prefix,
  const std::string &fileName)
{
    /* Delete objects and file at end of test. */
    int ret = 0;
    for (int i = 0; i < totalObjects; i++) {
        if ((ret = conn.DeleteObject(prefix + std::to_string(i) + ".txt")) != 0)
            std::cerr << "Error in CleanupTestListBuckets: failed to remove "
                      << TestDefaults::objPrefix + prefix << std::to_string(i) << ".txt from "
                      << TestDefaults::bucketName << std::endl;
    }
    std::remove(fileName.c_str());

    return (ret);
}

/*
 * TestListObjects --
 *     Unit test for listing S3 objects under the test bucket.
 */
/* Todo: Remove code duplication in this function. */
int
TestListObjects(const Aws::S3Crt::ClientConfiguration &config)
{
    S3Connection conn(config, TestDefaults::bucketName, TestDefaults::objPrefix);
    std::vector<std::string> objects;

    /* Name of file to insert in the test. */
    const std::string fileName = "test_list_objects.txt";
    /* Total objects to insert in the test. */
    const int32_t totalObjects = 20;
    /* Prefix for objects in this test. */
    const std::string prefix = "test_list_objects_";
    /* Parameter for getting single object. */
    const bool listSingle = true;
    /* Number of objects to access per iteration of AWS. */
    int32_t batchSize = 1;
    /* Expected number of matches. */
    int32_t expectedResult = 0;

    int ret;
    /* No matching objects. */
    if ((ret = conn.ListObjects(prefix, objects)) != 0)
        return (ret);
    if (objects.size() != expectedResult)
        return (TEST_FAILURE);

    /* No matching objects with listSingle. */
    if ((ret = conn.ListObjects(prefix, objects, batchSize, listSingle)) != 0)
        return (ret);
    if (objects.size() != expectedResult)
        return (TEST_FAILURE);

    /* Create file to prepare for test. */
    if (!static_cast<bool>(std::ofstream(fileName).put('.'))) {
        std::cerr << "Error creating file." << std::endl;
        return (TEST_FAILURE);
    }

    /* Put objects to prepare for test. */
    for (int i = 0; i < totalObjects; i++) {
        if ((ret = conn.PutObject(prefix + std::to_string(i) + ".txt", fileName)) != 0) {
            CleanupTestListObjects(conn, i, prefix, fileName);
            return (ret);
        }
    }

    /* List all objects. */
    expectedResult = totalObjects;
    if ((ret = conn.ListObjects(prefix, objects)) != 0) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (ret);
    }
    if (objects.size() != expectedResult) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (TEST_FAILURE);
    }

    /* List single. */
    objects.clear();
    expectedResult = 1;
    if ((ret = conn.ListObjects(prefix, objects, batchSize, listSingle)) != 0) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (ret);
    }
    if (objects.size() != expectedResult) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (TEST_FAILURE);
    }

    /* Expected number of matches with test_list_objects_1 prefix. */
    objects.clear();
    expectedResult = 11;
    if ((ret = conn.ListObjects(prefix + "1", objects)) != 0) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (ret);
    }
    if (objects.size() != expectedResult) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (TEST_FAILURE);
    }

    /* List with 5 objects per AWS request. */
    objects.clear();
    batchSize = 5;
    expectedResult = totalObjects;
    if ((ret = conn.ListObjects(prefix, objects, batchSize)) != 0) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (ret);
    }
    if (objects.size() != expectedResult) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (TEST_FAILURE);
    }

    /* ListSingle with 8 objects per AWS request. */
    objects.clear();
    expectedResult = 1;
    if ((ret = conn.ListObjects(prefix, objects, batchSize, listSingle)) != 0) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (ret);
    }
    if (objects.size() != expectedResult) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (TEST_FAILURE);
    }

    /* List with 8 objects per AWS request. */
    objects.clear();
    batchSize = 8;
    expectedResult = totalObjects;
    if ((ret = conn.ListObjects(prefix, objects, batchSize)) != 0) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (ret);
    }
    if (objects.size() != expectedResult) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (TEST_FAILURE);
    }

    /* ListSingle with 8 objects per AWS request. */
    objects.clear();
    expectedResult = 1;
    if ((ret = conn.ListObjects(prefix, objects, batchSize, listSingle)) != 0) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (ret);
    }
    if (objects.size() != expectedResult) {
        CleanupTestListObjects(conn, totalObjects, prefix, fileName);
        return (TEST_FAILURE);
    }

    // CleanupTestListObjects(conn, totalObjects, prefix, fileName);
    std::cout << "TestListObjects(): succeeded." << std::endl;
    return (TEST_SUCCESS);
}

/*
 * TestGetObject --
 *     Unit test to get an object from an S3 Bucket.
 */
int
TestGetObject(const Aws::S3Crt::ClientConfiguration &config)
{
    S3Connection conn(config, TestDefaults::bucketName, TestDefaults::objPrefix);
    int ret = TEST_FAILURE;

    const std::string objectName = "permanent_object";
    const std::string path = "./" + objectName;

    /* Create a file and upload to the bucket. */
    std::ofstream File(objectName);
    File << "Test payload";
    File.close();
    if ((ret = conn.PutObject(objectName, objectName)) != 0)
        return (ret);

    /* Delete the local copy of the file. */
    if (std::remove(path.c_str()) != 0)
        return (TEST_FAILURE);

    /* Download the file from S3 */
    if ((ret = conn.GetObject(objectName, path)) != 0) {
        std::cerr << "TestGetObject: call to S3Connection:GetObject has failed." << std::endl;
        return (ret);
    }

    /* The file should now be in the current directory. */
    std::ifstream f(path);
    if (!f.good()) {
        std::cerr << "TestGetObject: target " << objectName
                  << " has not been successfully downloaded." << std::endl;
        return (TEST_FAILURE);
    }

    /* Clean up test artifacts. */
    if (std::remove(path.c_str()) != 0)
        return (TEST_FAILURE);

    if ((ret = conn.DeleteObject(objectName)) != 0)
        return (ret);

    std::cout << "TestGetObject() succeeded." << std::endl;
    return (TEST_SUCCESS);
}
/*
 * TestObjectExists --
 *     Unit test to check if an object exists in an AWS bucket.
 */
int
TestObjectExists(const Aws::S3Crt::ClientConfiguration &config)
{
    S3Connection conn(config, TestDefaults::bucketName, TestDefaults::objPrefix);
    bool exists = false;
    int ret = TEST_FAILURE;

    const std::string objectName = "test_object";
    const std::string fileName = "test_object.txt";

    /* Create a file to upload to the bucket.*/
    std::ofstream File(fileName);
    File << "Test payload";
    File.close();

    ret = conn.ObjectExists(objectName, exists);
    if (ret != 0 || exists)
        return (TEST_FAILURE);

    if ((ret = conn.PutObject(objectName, fileName)) != 0)
        return (ret);

    ret = conn.ObjectExists(objectName, exists);
    if (ret != 0 || !exists)
        return (TEST_FAILURE);

    if ((ret = conn.DeleteObject(objectName)) != 0)
        return (ret);
    std::cout << "TestObjectExists() succeeded." << std::endl;
    return (TEST_SUCCESS);
}

/*
 * main --
 *     Set up configs and call unit tests.
 */
int
main()
{
    /* Setup the test environment. */
    if (setupTestDefaults() != 0)
        return (TEST_FAILURE);

    /* Set up the config to use the defaults specified. */
    Aws::S3Crt::ClientConfiguration awsConfig;
    awsConfig.region = TestDefaults::region;
    awsConfig.throughputTargetGbps = TestDefaults::throughputTargetGbps;
    awsConfig.partSize = TestDefaults::partSize;

    /* Set the SDK options and initialize the API. */
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    TEST(TestObjectExists, awsConfig);
    TEST(TestListObjects, awsConfig);
    TEST(TestGetObject, awsConfig);

    /* Shutdown the API at end of tests. */
    Aws::ShutdownAPI(options);
    return (TEST_SUCCESS);
}
