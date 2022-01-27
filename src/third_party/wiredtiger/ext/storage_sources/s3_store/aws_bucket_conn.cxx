#include <aws/core/Aws.h>
#include <aws/s3-crt/model/DeleteObjectRequest.h>
#include <aws/s3-crt/model/ListObjectsRequest.h>
#include <aws/s3-crt/model/PutObjectRequest.h>
#include "aws_bucket_conn.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

/*
 * list_buckets --
 *     Builds a list of buckets from AWS account into a vector. Returns true if success, otherwise
 *     false.
 */
bool
aws_bucket_conn::list_buckets(std::vector<std::string> &buckets) const
{
    auto outcome = m_s3_crt_client.ListBuckets();
    if (outcome.IsSuccess()) {
        for (const auto &bucket : outcome.GetResult().GetBuckets())
            buckets.push_back(bucket.GetName());
        return true;
    } else {
        std::cerr << "Error in list_buckets: " << outcome.GetError().GetMessage() << std::endl
                  << std::endl;
        return false;
    }
}

/*
 * list_objects --
 *     Builds a list of object names from a S3 bucket into a vector. Returns true if success,
 *     otherwise false.
 */
bool
aws_bucket_conn::list_objects(
  const std::string &bucket_name, std::vector<std::string> &objects) const
{
    Aws::S3Crt::Model::ListObjectsRequest request;
    request.WithBucket(bucket_name);
    Aws::S3Crt::Model::ListObjectsOutcome outcomes = m_s3_crt_client.ListObjects(request);

    if (outcomes.IsSuccess()) {
        for (const auto &object : outcomes.GetResult().GetContents())
            objects.push_back(object.GetKey());
        return true;
    } else {
        std::cerr << "Error in list_buckets: " << outcomes.GetError().GetMessage() << std::endl;
        return false;
    }
}

/*
 * put_object --
 *     Puts an object into an S3 bucket. Returns true if success, otherwise false.
 */
bool
aws_bucket_conn::put_object(
  const std::string &bucket_name, const std::string &object_key, const std::string &file_name) const
{
    Aws::S3Crt::Model::PutObjectRequest request;
    request.SetBucket(bucket_name);
    request.SetKey(object_key);

    std::shared_ptr<Aws::IOStream> input_data = Aws::MakeShared<Aws::FStream>(
      "s3-source", file_name.c_str(), std::ios_base::in | std::ios_base::binary);

    request.SetBody(input_data);

    Aws::S3Crt::Model::PutObjectOutcome outcome = m_s3_crt_client.PutObject(request);

    if (outcome.IsSuccess()) {
        return true;
    } else {
        std::cerr << "Error in put_object: " << outcome.GetError().GetMessage() << std::endl;
        return false;
    }
}

/*
 * delete_object --
 *     Deletes an object from S3 bucket. Returns true if success, otherwise false.
 */
bool
aws_bucket_conn::delete_object(const std::string &bucket_name, const std::string &object_key) const
{
    Aws::S3Crt::Model::DeleteObjectRequest request;
    request.SetBucket(bucket_name);
    request.SetKey(object_key);

    Aws::S3Crt::Model::DeleteObjectOutcome outcome = m_s3_crt_client.DeleteObject(request);

    if (outcome.IsSuccess()) {
        return true;
    } else {
        std::cerr << "Error in delete_object: " << outcome.GetError().GetMessage() << std::endl;
        return false;
    }
}

/*
 * aws_bucket_conn --
 *     Constructor for AWS bucket connection.
 */
aws_bucket_conn::aws_bucket_conn(const Aws::S3Crt::ClientConfiguration &config)
    : m_s3_crt_client(config){};
