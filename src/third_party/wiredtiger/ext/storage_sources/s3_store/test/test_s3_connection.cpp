#include <s3_connection.h>
#include <fstream>

/* Default config settings for the S3CrtClient. */
namespace TestDefaults {
const Aws::String region = Aws::Region::AP_SOUTHEAST_2;
const double throughputTargetGbps = 5;
const uint64_t partSize = 8 * 1024 * 1024; /* 8 MB. */
} // namespace TestDefaults

int TestListBuckets(const Aws::S3Crt::ClientConfiguration &config);
int TestObjectExists(const Aws::S3Crt::ClientConfiguration &config);

/* Wrapper for unit test functions. */
#define TEST(func, config, expectedOutput)              \
    do {                                                \
        int __ret;                                      \
        if ((__ret = (func(config))) != expectedOutput) \
            return (__ret);                             \
    } while (0)

/*
 * TestListBuckets --
 *     Example of a unit test to list S3 buckets under the associated AWS account.
 */
int
TestListBuckets(const Aws::S3Crt::ClientConfiguration &config)
{
    S3Connection conn(config);
    std::vector<std::string> buckets;
    if (!conn.ListBuckets(buckets))
        return 1;

    std::cout << "All buckets under my account:" << std::endl;
    for (const auto &bucket : buckets)
        std::cout << "  * " << bucket << std::endl;
    return 0;
}

/*
 * TestObjectExists --
 *     Unit test to check if an object exists in an AWS bucket.
 */
int
TestObjectExists(const Aws::S3Crt::ClientConfiguration &config)
{
    S3Connection conn(config);
    std::vector<std::string> buckets;
    bool exists = false;
    int ret = 1;

    if (!conn.ListBuckets(buckets))
        return 1;
    const std::string bucketName = buckets.at(0);
    const std::string objectName = "test_object";
    const std::string fileName = "test_object.txt";

    /* Create a file to upload to the bucket.*/
    std::ofstream File(fileName);
    File << "Test payload";
    File.close();

    if ((ret = conn.ObjectExists(bucketName, objectName, exists)) != 0 || exists)
        return (ret);

    if (!(conn.PutObject(bucketName, objectName, fileName)))
        return (1);

    if ((ret = conn.ObjectExists(bucketName, objectName, exists)) != 0 || !exists)
        return (ret);

    if (!(conn.DeleteObject(bucketName, objectName)))
        return (1);
    std::cout << "TestObjectExists(): succeeded.\n" << std::endl;
    return 0;
}

/*
 * main --
 *     Set up configs and call unit tests.
 */
int
main()
{
    /* Set up the config to use the defaults specified. */
    Aws::S3Crt::ClientConfiguration awsConfig;
    awsConfig.region = TestDefaults::region;
    awsConfig.throughputTargetGbps = TestDefaults::throughputTargetGbps;
    awsConfig.partSize = TestDefaults::partSize;

    /* Set the SDK options and initialize the API. */
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    int expectedOutput = 0;
    TEST(TestListBuckets, awsConfig, expectedOutput);

    int objectExistsExpectedOutput = 0;
    TEST(TestObjectExists, awsConfig, objectExistsExpectedOutput);

    /* Shutdown the API at end of tests. */
    Aws::ShutdownAPI(options);
    return 0;
}
