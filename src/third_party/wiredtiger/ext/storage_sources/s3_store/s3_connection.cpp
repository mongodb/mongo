#include <aws/core/Aws.h>
#include <aws/s3-crt/model/DeleteObjectRequest.h>
#include <aws/s3-crt/model/ListObjectsRequest.h>
#include <aws/s3-crt/model/PutObjectRequest.h>
#include <aws/s3-crt/model/HeadObjectRequest.h>

#include "s3_connection.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

/*
 * ListBuckets --
 *     Builds a list of buckets from AWS account into a vector. Returns true if success, otherwise
 *     false.
 */
bool
S3Connection::ListBuckets(std::vector<std::string> &buckets) const
{
    auto outcome = m_S3CrtClient.ListBuckets();
    if (outcome.IsSuccess()) {
        for (const auto &bucket : outcome.GetResult().GetBuckets())
            buckets.push_back(bucket.GetName());
        return true;
    } else {
        std::cerr << "Error in ListBuckets: " << outcome.GetError().GetMessage() << std::endl
                  << std::endl;
        return false;
    }
}

/*
 * ListObjects --
 *     Builds a list of object names from a S3 bucket into a vector. Returns true if success,
 *     otherwise false.
 */
bool
S3Connection::ListObjects(const std::string &bucketName, std::vector<std::string> &objects) const
{
    Aws::S3Crt::Model::ListObjectsRequest request;
    request.WithBucket(bucketName);
    Aws::S3Crt::Model::ListObjectsOutcome outcomes = m_S3CrtClient.ListObjects(request);

    if (outcomes.IsSuccess()) {
        for (const auto &object : outcomes.GetResult().GetContents())
            objects.push_back(object.GetKey());
        return true;
    } else {
        std::cerr << "Error in ListObjects: " << outcomes.GetError().GetMessage() << std::endl;
        return false;
    }
}

/*
 * PutObject --
 *     Puts an object into an S3 bucket. Returns true if success, otherwise false.
 */
bool
S3Connection::PutObject(
  const std::string &bucketName, const std::string &objectKey, const std::string &fileName) const
{
    Aws::S3Crt::Model::PutObjectRequest request;
    request.SetBucket(bucketName);
    request.SetKey(objectKey);

    std::shared_ptr<Aws::IOStream> inputData = Aws::MakeShared<Aws::FStream>(
      "s3-source", fileName.c_str(), std::ios_base::in | std::ios_base::binary);

    request.SetBody(inputData);

    Aws::S3Crt::Model::PutObjectOutcome outcome = m_S3CrtClient.PutObject(request);

    if (outcome.IsSuccess()) {
        return true;
    } else {
        std::cerr << "Error in PutObject: " << outcome.GetError().GetMessage() << std::endl;
        return false;
    }
}

/*
 * DeleteObject --
 *     Deletes an object from S3 bucket. Returns true if success, otherwise false.
 */
bool
S3Connection::DeleteObject(const std::string &bucketName, const std::string &objectKey) const
{
    Aws::S3Crt::Model::DeleteObjectRequest request;
    request.SetBucket(bucketName);
    request.SetKey(objectKey);

    Aws::S3Crt::Model::DeleteObjectOutcome outcome = m_S3CrtClient.DeleteObject(request);

    if (outcome.IsSuccess()) {
        return true;
    } else {
        std::cerr << "Error in DeleteObject: " << outcome.GetError().GetMessage() << std::endl;
        return false;
    }
}

/*
 * ObjectExists --
 *     Checks whether an object with the given key exists in the S3 bucket.
 */
int
S3Connection::ObjectExists(
  const std::string &bucketName, const std::string &objectKey, bool &exists) const
{
    exists = false;

    Aws::S3Crt::Model::HeadObjectRequest request;
    request.SetBucket(bucketName);
    request.SetKey(objectKey);
    Aws::S3Crt::Model::HeadObjectOutcome outcome = m_S3CrtClient.HeadObject(request);

    /*
     * If an object with the given key does not exist the HEAD request will return a 404.
     * https://docs.aws.amazon.com/AmazonS3/latest/API/API_HeadObject.html Do not fail in this case.
     */
    if (outcome.IsSuccess()) {
        exists = true;
        return (0);
    } else if (outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND)
        return (0);
    else
        return (-1);
}

/*
 * S3Connection --
 *     Constructor for AWS S3 bucket connection.
 */
S3Connection::S3Connection(const Aws::S3Crt::ClientConfiguration &config) : m_S3CrtClient(config){};
