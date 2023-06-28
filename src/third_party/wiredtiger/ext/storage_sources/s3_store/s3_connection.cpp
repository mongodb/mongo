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
#include "s3_connection.h"

#include <aws/s3-crt/model/DeleteObjectRequest.h>
#include <aws/s3-crt/model/ListObjectsV2Request.h>
#include <aws/s3-crt/model/PutObjectRequest.h>
#include <aws/s3-crt/model/GetObjectRequest.h>
#include <aws/s3-crt/model/HeadObjectRequest.h>
#include <aws/s3-crt/model/HeadBucketRequest.h>

#include <fstream>
#include <iostream>

// Constructor for AWS S3 bucket connection with provided credentials.
S3Connection::S3Connection(const Aws::Auth::AWSCredentials &credentials,
  const Aws::S3Crt::ClientConfiguration &config, const std::string &bucketName,
  const std::string &objPrefix)
    : _s3CrtClient(credentials, config), _bucketName(bucketName), _objectPrefix(objPrefix)
{
    // Confirm that we can access the bucket, else fail.
    bool exists;
    int ret = BucketExists(exists);
    if (!exists)
        throw std::invalid_argument(_bucketName + " : No such bucket.");
    if (ret != 0)
        throw std::invalid_argument(_bucketName + " :Unable to access bucket.");
}

// Constructor for AWS S3 bucket connection with credentials in local file.
S3Connection::S3Connection(const Aws::S3Crt::ClientConfiguration &config,
  const std::string &bucketName, const std::string &objPrefix)
    : _s3CrtClient(config), _bucketName(bucketName), _objectPrefix(objPrefix)
{
    // Confirm that we can access the bucket, else fail.
    bool exists;
    int ret = BucketExists(exists);
    if (!exists)
        throw std::invalid_argument(_bucketName + " : No such bucket.");
    if (ret != 0)
        throw std::invalid_argument(_bucketName + " : Unable to access bucket.");
}

/*
 * Builds a list of object names, with prefix matching, from an S3 bucket into a vector. The
 * batchSize parameter specifies the maximum number of objects returned in each AWS response, up to
 * 1000. Return an errno value given an HTTP response code if the aws request does not succeed.
 */
int
S3Connection::ListObjects(const std::string &prefix, std::vector<std::string> &objects,
  uint32_t batchSize, bool listSingle) const
{
    Aws::S3Crt::Model::ListObjectsV2Request request;
    request.SetBucket(_bucketName);
    request.SetPrefix(_objectPrefix + prefix);
    listSingle ? request.SetMaxKeys(1) : request.SetMaxKeys(batchSize);

    Aws::S3Crt::Model::ListObjectsV2Outcome outcomes = _s3CrtClient.ListObjectsV2(request);
    if (!outcomes.IsSuccess()) {
        Aws::Http::HttpResponseCode resCode = outcomes.GetError().GetResponseCode();
        if (toErrno.find(resCode) != toErrno.end())
            return (toErrno.at(resCode));
        return (-1);
    }
    auto result = outcomes.GetResult();

    // Returning the object name with the prefix stripped.
    for (const auto &object : result.GetContents())
        objects.push_back(object.GetKey().substr(_objectPrefix.length()));

    if (listSingle)
        return (0);

    // Continuation token will be an empty string if we have returned all possible objects.
    std::string continuationToken = result.GetNextContinuationToken();
    while (continuationToken != "") {
        request.SetContinuationToken(continuationToken);
        outcomes = _s3CrtClient.ListObjectsV2(request);
        if (!outcomes.IsSuccess()) {
            Aws::Http::HttpResponseCode resCode = outcomes.GetError().GetResponseCode();
            if (toErrno.find(resCode) != toErrno.end())
                return (toErrno.at(resCode));
            return (-1);
        }
        result = outcomes.GetResult();
        for (const auto &object : result.GetContents())
            objects.push_back(object.GetKey().substr(_objectPrefix.length()));
        continuationToken = result.GetNextContinuationToken();
    }
    return (0);
}

// Puts an object into an S3 bucket. Return an errno value given an HTTP response code if
// the aws request does not succeed.
int
S3Connection::PutObject(const std::string &objectKey, const std::string &fileName) const
{
    std::shared_ptr<Aws::IOStream> inputData = Aws::MakeShared<Aws::FStream>(
      s3AllocationTag, fileName.c_str(), std::ios_base::in | std::ios_base::binary);

    Aws::S3Crt::Model::PutObjectRequest request;
    request.SetBucket(_bucketName);
    request.SetKey(_objectPrefix + objectKey);
    request.SetBody(inputData);

    Aws::S3Crt::Model::PutObjectOutcome outcome = _s3CrtClient.PutObject(request);

    if (outcome.IsSuccess())
        return (0);

    Aws::Http::HttpResponseCode resCode = outcome.GetError().GetResponseCode();
    if (toErrno.find(resCode) != toErrno.end())
        return (toErrno.at(resCode));

    return (-1);
}

// Deletes an object from S3 bucket. Return an errno value given an HTTP response code if
// the aws request does not succeed.
int
S3Connection::DeleteObject(const std::string &objectKey) const
{
    Aws::S3Crt::Model::DeleteObjectRequest request;
    request.SetBucket(_bucketName);
    request.SetKey(_objectPrefix + objectKey);

    Aws::S3Crt::Model::DeleteObjectOutcome outcome = _s3CrtClient.DeleteObject(request);

    if (outcome.IsSuccess())
        return (0);

    Aws::Http::HttpResponseCode resCode = outcome.GetError().GetResponseCode();
    if (toErrno.find(resCode) != toErrno.end())
        return (toErrno.at(resCode));

    return (-1);
}

// Retrieves an object from S3. The object is downloaded to disk at the specified location.
int
S3Connection::GetObject(const std::string &objectKey, const std::string &path) const
{
    Aws::S3Crt::Model::GetObjectRequest request;
    request.SetBucket(_bucketName);
    request.SetKey(_objectPrefix + objectKey);

    // The S3 Object should be downloaded to disk rather than into an in-memory buffer. Use a custom
    // response stream factory to specify how the response should be downloaded.
    request.SetResponseStreamFactory([=]() {
        return (Aws::New<Aws::FStream>(
          s3AllocationTag, path, std::ios_base::out | std::ios_base::binary));
    });

    Aws::S3Crt::Model::GetObjectOutcome outcome = _s3CrtClient.GetObject(request);

    // Return an errno value given an HTTP response code if the aws request does not
    // succeed.
    if (outcome.IsSuccess())
        return (0);

    Aws::Http::HttpResponseCode resCode = outcome.GetError().GetResponseCode();
    if (toErrno.find(resCode) != toErrno.end())
        return (toErrno.at(resCode));

    return (-1);
}

// Checks whether an object with the given key exists in the S3 bucket and also retrieves
// size of the object.
int
S3Connection::ObjectExists(const std::string &objectKey, bool &exists, size_t &objectSize) const
{
    exists = false;
    objectSize = 0;

    Aws::S3Crt::Model::HeadObjectRequest request;
    request.SetBucket(_bucketName);
    request.SetKey(_objectPrefix + objectKey);
    Aws::S3Crt::Model::HeadObjectOutcome outcome = _s3CrtClient.HeadObject(request);

    /*
     * If an object with the given key does not exist the HEAD request will return a 404. Do not
     * fail in this case as it is an expected response. Otherwise return an errno value for any
     * other HTTP response code.
     */
    if (outcome.IsSuccess()) {
        exists = true;
        objectSize = outcome.GetResult().GetContentLength();
        return (0);
    }

    Aws::Http::HttpResponseCode resCode = outcome.GetError().GetResponseCode();
    if (resCode == Aws::Http::HttpResponseCode::NOT_FOUND)
        return (0);
    if (toErrno.find(resCode) != toErrno.end())
        return (toErrno.at(resCode));

    return (-1);
}

// Checks whether the bucket configured for the class is accessible to us or not.
int
S3Connection::BucketExists(bool &exists) const
{
    exists = false;

    Aws::S3Crt::Model::HeadBucketRequest request;
    request.WithBucket(_bucketName);
    Aws::S3Crt::Model::HeadBucketOutcome outcome = _s3CrtClient.HeadBucket(request);

    /*
     * If an object with the given key does not exist the HEAD request will return a 404. Do not
     * fail in this case as it is an expected response. Otherwise return an errno value for any
     * other HTTP response code.
     */
    if (outcome.IsSuccess()) {
        exists = true;
        return (0);
    }

    Aws::Http::HttpResponseCode resCode = outcome.GetError().GetResponseCode();
    if (resCode == Aws::Http::HttpResponseCode::NOT_FOUND)
        return (0);
    if (toErrno.find(resCode) != toErrno.end())
        return (toErrno.at(resCode));

    return (-1);
}
