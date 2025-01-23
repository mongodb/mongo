/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/EncodingType.h>
#include <aws/s3/model/RequestPayer.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <utility>

namespace Aws
{
namespace Http
{
    class URI;
} //namespace Http
namespace S3
{
namespace Model
{

  /**
   */
  class ListMultipartUploadsRequest : public S3Request
  {
  public:
    AWS_S3_API ListMultipartUploadsRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListMultipartUploads"; }

    AWS_S3_API Aws::String SerializePayload() const override;

    AWS_S3_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;

    AWS_S3_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    AWS_S3_API bool HasEmbeddedError(IOStream &body, const Http::HeaderValueCollection &header) const override;
    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_S3_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the bucket to which the multipart upload was initiated. </p> <p>
     * <b>Directory buckets</b> - When you use this operation with a directory bucket,
     * you must use virtual-hosted-style requests in the format <code>
     * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.
     * Path-style requests are not supported. Directory bucket names must be unique in
     * the chosen Zone (Availability Zone or Local Zone). Bucket names must follow the
     * format <code> <i>bucket-base-name</i>--<i>zone-id</i>--x-s3</code> (for example,
     * <code> <i>DOC-EXAMPLE-BUCKET</i>--<i>usw2-az1</i>--x-s3</code>). For information
     * about bucket naming restrictions, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/directory-bucket-naming-rules.html">Directory
     * bucket naming rules</a> in the <i>Amazon S3 User Guide</i>.</p> <p> <b>Access
     * points</b> - When you use this action with an access point, you must provide the
     * alias of the access point in place of the bucket name or specify the access
     * point ARN. When using the access point ARN, you must direct requests to the
     * access point hostname. The access point hostname takes the form
     * <i>AccessPointName</i>-<i>AccountId</i>.s3-accesspoint.<i>Region</i>.amazonaws.com.
     * When using this action with an access point through the Amazon Web Services
     * SDKs, you provide the access point ARN in place of the bucket name. For more
     * information about access point ARNs, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-access-points.html">Using
     * access points</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>Access
     * points and Object Lambda access points are not supported by directory
     * buckets.</p>  <p> <b>S3 on Outposts</b> - When you use this action with
     * Amazon S3 on Outposts, you must direct requests to the S3 on Outposts hostname.
     * The S3 on Outposts hostname takes the form <code>
     * <i>AccessPointName</i>-<i>AccountId</i>.<i>outpostID</i>.s3-outposts.<i>Region</i>.amazonaws.com</code>.
     * When you use this action with S3 on Outposts through the Amazon Web Services
     * SDKs, you provide the Outposts access point ARN in place of the bucket name. For
     * more information about S3 on Outposts ARNs, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/S3onOutposts.html">What
     * is S3 on Outposts?</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline bool BucketHasBeenSet() const { return m_bucketHasBeenSet; }
    inline void SetBucket(const Aws::String& value) { m_bucketHasBeenSet = true; m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucketHasBeenSet = true; m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucketHasBeenSet = true; m_bucket.assign(value); }
    inline ListMultipartUploadsRequest& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline ListMultipartUploadsRequest& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline ListMultipartUploadsRequest& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Character you use to group keys.</p> <p>All keys that contain the same string
     * between the prefix, if specified, and the first occurrence of the delimiter
     * after the prefix are grouped under a single result element,
     * <code>CommonPrefixes</code>. If you don't specify the prefix parameter, then the
     * substring starts at the beginning of the key. The keys that are grouped under
     * <code>CommonPrefixes</code> result element are not returned elsewhere in the
     * response.</p>  <p> <b>Directory buckets</b> - For directory buckets,
     * <code>/</code> is the only supported delimiter.</p> 
     */
    inline const Aws::String& GetDelimiter() const{ return m_delimiter; }
    inline bool DelimiterHasBeenSet() const { return m_delimiterHasBeenSet; }
    inline void SetDelimiter(const Aws::String& value) { m_delimiterHasBeenSet = true; m_delimiter = value; }
    inline void SetDelimiter(Aws::String&& value) { m_delimiterHasBeenSet = true; m_delimiter = std::move(value); }
    inline void SetDelimiter(const char* value) { m_delimiterHasBeenSet = true; m_delimiter.assign(value); }
    inline ListMultipartUploadsRequest& WithDelimiter(const Aws::String& value) { SetDelimiter(value); return *this;}
    inline ListMultipartUploadsRequest& WithDelimiter(Aws::String&& value) { SetDelimiter(std::move(value)); return *this;}
    inline ListMultipartUploadsRequest& WithDelimiter(const char* value) { SetDelimiter(value); return *this;}
    ///@}

    ///@{
    
    inline const EncodingType& GetEncodingType() const{ return m_encodingType; }
    inline bool EncodingTypeHasBeenSet() const { return m_encodingTypeHasBeenSet; }
    inline void SetEncodingType(const EncodingType& value) { m_encodingTypeHasBeenSet = true; m_encodingType = value; }
    inline void SetEncodingType(EncodingType&& value) { m_encodingTypeHasBeenSet = true; m_encodingType = std::move(value); }
    inline ListMultipartUploadsRequest& WithEncodingType(const EncodingType& value) { SetEncodingType(value); return *this;}
    inline ListMultipartUploadsRequest& WithEncodingType(EncodingType&& value) { SetEncodingType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the multipart upload after which listing should begin.</p> 
     * <ul> <li> <p> <b>General purpose buckets</b> - For general purpose buckets,
     * <code>key-marker</code> is an object key. Together with
     * <code>upload-id-marker</code>, this parameter specifies the multipart upload
     * after which listing should begin.</p> <p>If <code>upload-id-marker</code> is not
     * specified, only the keys lexicographically greater than the specified
     * <code>key-marker</code> will be included in the list.</p> <p>If
     * <code>upload-id-marker</code> is specified, any multipart uploads for a key
     * equal to the <code>key-marker</code> might also be included, provided those
     * multipart uploads have upload IDs lexicographically greater than the specified
     * <code>upload-id-marker</code>.</p> </li> <li> <p> <b>Directory buckets</b> - For
     * directory buckets, <code>key-marker</code> is obfuscated and isn't a real object
     * key. The <code>upload-id-marker</code> parameter isn't supported by directory
     * buckets. To list the additional multipart uploads, you only need to set the
     * value of <code>key-marker</code> to the <code>NextKeyMarker</code> value from
     * the previous response. </p> <p>In the <code>ListMultipartUploads</code>
     * response, the multipart uploads aren't sorted lexicographically based on the
     * object keys. </p> </li> </ul> 
     */
    inline const Aws::String& GetKeyMarker() const{ return m_keyMarker; }
    inline bool KeyMarkerHasBeenSet() const { return m_keyMarkerHasBeenSet; }
    inline void SetKeyMarker(const Aws::String& value) { m_keyMarkerHasBeenSet = true; m_keyMarker = value; }
    inline void SetKeyMarker(Aws::String&& value) { m_keyMarkerHasBeenSet = true; m_keyMarker = std::move(value); }
    inline void SetKeyMarker(const char* value) { m_keyMarkerHasBeenSet = true; m_keyMarker.assign(value); }
    inline ListMultipartUploadsRequest& WithKeyMarker(const Aws::String& value) { SetKeyMarker(value); return *this;}
    inline ListMultipartUploadsRequest& WithKeyMarker(Aws::String&& value) { SetKeyMarker(std::move(value)); return *this;}
    inline ListMultipartUploadsRequest& WithKeyMarker(const char* value) { SetKeyMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Sets the maximum number of multipart uploads, from 1 to 1,000, to return in
     * the response body. 1,000 is the maximum number of uploads that can be returned
     * in a response.</p>
     */
    inline int GetMaxUploads() const{ return m_maxUploads; }
    inline bool MaxUploadsHasBeenSet() const { return m_maxUploadsHasBeenSet; }
    inline void SetMaxUploads(int value) { m_maxUploadsHasBeenSet = true; m_maxUploads = value; }
    inline ListMultipartUploadsRequest& WithMaxUploads(int value) { SetMaxUploads(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Lists in-progress uploads only for those keys that begin with the specified
     * prefix. You can use prefixes to separate a bucket into different grouping of
     * keys. (You can think of using <code>prefix</code> to make groups in the same way
     * that you'd use a folder in a file system.)</p>  <p> <b>Directory
     * buckets</b> - For directory buckets, only prefixes that end in a delimiter
     * (<code>/</code>) are supported.</p> 
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline ListMultipartUploadsRequest& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ListMultipartUploadsRequest& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ListMultipartUploadsRequest& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Together with key-marker, specifies the multipart upload after which listing
     * should begin. If key-marker is not specified, the upload-id-marker parameter is
     * ignored. Otherwise, any multipart uploads for a key equal to the key-marker
     * might be included in the list only if they have an upload ID lexicographically
     * greater than the specified <code>upload-id-marker</code>.</p>  <p>This
     * functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetUploadIdMarker() const{ return m_uploadIdMarker; }
    inline bool UploadIdMarkerHasBeenSet() const { return m_uploadIdMarkerHasBeenSet; }
    inline void SetUploadIdMarker(const Aws::String& value) { m_uploadIdMarkerHasBeenSet = true; m_uploadIdMarker = value; }
    inline void SetUploadIdMarker(Aws::String&& value) { m_uploadIdMarkerHasBeenSet = true; m_uploadIdMarker = std::move(value); }
    inline void SetUploadIdMarker(const char* value) { m_uploadIdMarkerHasBeenSet = true; m_uploadIdMarker.assign(value); }
    inline ListMultipartUploadsRequest& WithUploadIdMarker(const Aws::String& value) { SetUploadIdMarker(value); return *this;}
    inline ListMultipartUploadsRequest& WithUploadIdMarker(Aws::String&& value) { SetUploadIdMarker(std::move(value)); return *this;}
    inline ListMultipartUploadsRequest& WithUploadIdMarker(const char* value) { SetUploadIdMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The account ID of the expected bucket owner. If the account ID that you
     * provide does not match the actual owner of the bucket, the request fails with
     * the HTTP status code <code>403 Forbidden</code> (access denied).</p>
     */
    inline const Aws::String& GetExpectedBucketOwner() const{ return m_expectedBucketOwner; }
    inline bool ExpectedBucketOwnerHasBeenSet() const { return m_expectedBucketOwnerHasBeenSet; }
    inline void SetExpectedBucketOwner(const Aws::String& value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner = value; }
    inline void SetExpectedBucketOwner(Aws::String&& value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner = std::move(value); }
    inline void SetExpectedBucketOwner(const char* value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner.assign(value); }
    inline ListMultipartUploadsRequest& WithExpectedBucketOwner(const Aws::String& value) { SetExpectedBucketOwner(value); return *this;}
    inline ListMultipartUploadsRequest& WithExpectedBucketOwner(Aws::String&& value) { SetExpectedBucketOwner(std::move(value)); return *this;}
    inline ListMultipartUploadsRequest& WithExpectedBucketOwner(const char* value) { SetExpectedBucketOwner(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestPayer& GetRequestPayer() const{ return m_requestPayer; }
    inline bool RequestPayerHasBeenSet() const { return m_requestPayerHasBeenSet; }
    inline void SetRequestPayer(const RequestPayer& value) { m_requestPayerHasBeenSet = true; m_requestPayer = value; }
    inline void SetRequestPayer(RequestPayer&& value) { m_requestPayerHasBeenSet = true; m_requestPayer = std::move(value); }
    inline ListMultipartUploadsRequest& WithRequestPayer(const RequestPayer& value) { SetRequestPayer(value); return *this;}
    inline ListMultipartUploadsRequest& WithRequestPayer(RequestPayer&& value) { SetRequestPayer(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline ListMultipartUploadsRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline ListMultipartUploadsRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline ListMultipartUploadsRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline ListMultipartUploadsRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline ListMultipartUploadsRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline ListMultipartUploadsRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline ListMultipartUploadsRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline ListMultipartUploadsRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline ListMultipartUploadsRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Aws::String m_delimiter;
    bool m_delimiterHasBeenSet = false;

    EncodingType m_encodingType;
    bool m_encodingTypeHasBeenSet = false;

    Aws::String m_keyMarker;
    bool m_keyMarkerHasBeenSet = false;

    int m_maxUploads;
    bool m_maxUploadsHasBeenSet = false;

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    Aws::String m_uploadIdMarker;
    bool m_uploadIdMarkerHasBeenSet = false;

    Aws::String m_expectedBucketOwner;
    bool m_expectedBucketOwnerHasBeenSet = false;

    RequestPayer m_requestPayer;
    bool m_requestPayerHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
