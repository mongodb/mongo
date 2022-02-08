
#ifndef S3CONNECTION
#define S3CONNECTION

#include <aws/core/Aws.h>
#include <aws/s3-crt/S3CrtClient.h>

#include <string>
#include <vector>

/*
 * Class to represent an active connection to the AWS S3 endpoint. Allows for interaction with S3
 * client.
 */
class S3Connection {
    public:
    explicit S3Connection(const Aws::S3Crt::ClientConfiguration &config);
    bool ListBuckets(std::vector<std::string> &buckets) const;
    bool ListObjects(const std::string &bucketName, std::vector<std::string> &objects) const;
    bool PutObject(const std::string &bucketName, const std::string &objectKey,
      const std::string &fileName) const;
    bool DeleteObject(const std::string &bucketName, const std::string &objectKey) const;
    int ObjectExists(
      const std::string &bucketName, const std::string &objectKey, bool &exists) const;
    ~S3Connection() = default;

    private:
    const Aws::S3Crt::S3CrtClient m_S3CrtClient;
};
#endif
