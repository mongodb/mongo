
#ifndef S3CONNECTION
#define S3CONNECTION

#include <aws/auth/credentials.h>
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/s3-crt/S3CrtClient.h>

#include <string>
#include <vector>

/*
 * Class to represent an active connection to the AWS S3 endpoint. Allows for interaction with S3
 * client.
 */
class S3Connection {
    public:
    S3Connection(const Aws::Auth::AWSCredentials &credentials,
      const Aws::S3Crt::ClientConfiguration &config, const std::string &bucketName,
      const std::string &objPrefix = "");
    S3Connection(const Aws::S3Crt::ClientConfiguration &config, const std::string &bucketName,
      const std::string &objPrefix = "");
    int ListObjects(const std::string &prefix, std::vector<std::string> &objects,
      uint32_t batchSize = 1000, bool listSingle = false) const;
    int PutObject(const std::string &objectKey, const std::string &fileName) const;
    int DeleteObject(const std::string &objectKey) const;
    int ObjectExists(const std::string &objectKey, bool &exists, size_t &objectSize) const;
    int GetObject(const std::string &objectKey, const std::string &path) const;

    ~S3Connection() = default;

    private:
    const Aws::S3Crt::S3CrtClient _s3CrtClient;
    const std::string _bucketName;
    const std::string _objectPrefix;

    int BucketExists(bool &exists) const;
};
#endif
