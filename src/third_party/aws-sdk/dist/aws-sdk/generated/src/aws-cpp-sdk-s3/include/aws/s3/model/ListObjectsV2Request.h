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
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/s3/model/OptionalObjectAttributes.h>
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
  class ListObjectsV2Request : public S3Request
  {
  public:
    AWS_S3_API ListObjectsV2Request();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListObjectsV2"; }

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
     * <p> <b>Directory buckets</b> - When you use this operation with a directory
     * bucket, you must use virtual-hosted-style requests in the format <code>
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
    inline ListObjectsV2Request& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline ListObjectsV2Request& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline ListObjectsV2Request& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A delimiter is a character that you use to group keys.</p>  <ul> <li>
     * <p> <b>Directory buckets</b> - For directory buckets, <code>/</code> is the only
     * supported delimiter.</p> </li> <li> <p> <b>Directory buckets </b> - When you
     * query <code>ListObjectsV2</code> with a delimiter during in-progress multipart
     * uploads, the <code>CommonPrefixes</code> response parameter contains the
     * prefixes that are associated with the in-progress multipart uploads. For more
     * information about multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuoverview.html">Multipart
     * Upload Overview</a> in the <i>Amazon S3 User Guide</i>.</p> </li> </ul> 
     */
    inline const Aws::String& GetDelimiter() const{ return m_delimiter; }
    inline bool DelimiterHasBeenSet() const { return m_delimiterHasBeenSet; }
    inline void SetDelimiter(const Aws::String& value) { m_delimiterHasBeenSet = true; m_delimiter = value; }
    inline void SetDelimiter(Aws::String&& value) { m_delimiterHasBeenSet = true; m_delimiter = std::move(value); }
    inline void SetDelimiter(const char* value) { m_delimiterHasBeenSet = true; m_delimiter.assign(value); }
    inline ListObjectsV2Request& WithDelimiter(const Aws::String& value) { SetDelimiter(value); return *this;}
    inline ListObjectsV2Request& WithDelimiter(Aws::String&& value) { SetDelimiter(std::move(value)); return *this;}
    inline ListObjectsV2Request& WithDelimiter(const char* value) { SetDelimiter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Encoding type used by Amazon S3 to encode the <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html">object
     * keys</a> in the response. Responses are encoded only in UTF-8. An object key can
     * contain any Unicode character. However, the XML 1.0 parser can't parse certain
     * characters, such as characters with an ASCII value from 0 to 10. For characters
     * that aren't supported in XML 1.0, you can add this parameter to request that
     * Amazon S3 encode the keys in the response. For more information about characters
     * to avoid in object key names, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html#object-key-guidelines">Object
     * key naming guidelines</a>.</p>  <p>When using the URL encoding type,
     * non-ASCII characters that are used in an object's key name will be
     * percent-encoded according to UTF-8 code values. For example, the object
     * <code>test_file(3).png</code> will appear as
     * <code>test_file%283%29.png</code>.</p> 
     */
    inline const EncodingType& GetEncodingType() const{ return m_encodingType; }
    inline bool EncodingTypeHasBeenSet() const { return m_encodingTypeHasBeenSet; }
    inline void SetEncodingType(const EncodingType& value) { m_encodingTypeHasBeenSet = true; m_encodingType = value; }
    inline void SetEncodingType(EncodingType&& value) { m_encodingTypeHasBeenSet = true; m_encodingType = std::move(value); }
    inline ListObjectsV2Request& WithEncodingType(const EncodingType& value) { SetEncodingType(value); return *this;}
    inline ListObjectsV2Request& WithEncodingType(EncodingType&& value) { SetEncodingType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Sets the maximum number of keys returned in the response. By default, the
     * action returns up to 1,000 key names. The response might contain fewer keys but
     * will never contain more.</p>
     */
    inline int GetMaxKeys() const{ return m_maxKeys; }
    inline bool MaxKeysHasBeenSet() const { return m_maxKeysHasBeenSet; }
    inline void SetMaxKeys(int value) { m_maxKeysHasBeenSet = true; m_maxKeys = value; }
    inline ListObjectsV2Request& WithMaxKeys(int value) { SetMaxKeys(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Limits the response to keys that begin with the specified prefix.</p> 
     * <p> <b>Directory buckets</b> - For directory buckets, only prefixes that end in
     * a delimiter (<code>/</code>) are supported.</p> 
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline ListObjectsV2Request& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ListObjectsV2Request& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ListObjectsV2Request& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> <code>ContinuationToken</code> indicates to Amazon S3 that the list is being
     * continued on this bucket with a token. <code>ContinuationToken</code> is
     * obfuscated and is not a real key. You can use this
     * <code>ContinuationToken</code> for pagination of the list results. </p>
     */
    inline const Aws::String& GetContinuationToken() const{ return m_continuationToken; }
    inline bool ContinuationTokenHasBeenSet() const { return m_continuationTokenHasBeenSet; }
    inline void SetContinuationToken(const Aws::String& value) { m_continuationTokenHasBeenSet = true; m_continuationToken = value; }
    inline void SetContinuationToken(Aws::String&& value) { m_continuationTokenHasBeenSet = true; m_continuationToken = std::move(value); }
    inline void SetContinuationToken(const char* value) { m_continuationTokenHasBeenSet = true; m_continuationToken.assign(value); }
    inline ListObjectsV2Request& WithContinuationToken(const Aws::String& value) { SetContinuationToken(value); return *this;}
    inline ListObjectsV2Request& WithContinuationToken(Aws::String&& value) { SetContinuationToken(std::move(value)); return *this;}
    inline ListObjectsV2Request& WithContinuationToken(const char* value) { SetContinuationToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The owner field is not present in <code>ListObjectsV2</code> by default. If
     * you want to return the owner field with each key in the result, then set the
     * <code>FetchOwner</code> field to <code>true</code>.</p>  <p> <b>Directory
     * buckets</b> - For directory buckets, the bucket owner is returned as the object
     * owner for all objects.</p> 
     */
    inline bool GetFetchOwner() const{ return m_fetchOwner; }
    inline bool FetchOwnerHasBeenSet() const { return m_fetchOwnerHasBeenSet; }
    inline void SetFetchOwner(bool value) { m_fetchOwnerHasBeenSet = true; m_fetchOwner = value; }
    inline ListObjectsV2Request& WithFetchOwner(bool value) { SetFetchOwner(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>StartAfter is where you want Amazon S3 to start listing from. Amazon S3
     * starts listing after this specified key. StartAfter can be any key in the
     * bucket.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const Aws::String& GetStartAfter() const{ return m_startAfter; }
    inline bool StartAfterHasBeenSet() const { return m_startAfterHasBeenSet; }
    inline void SetStartAfter(const Aws::String& value) { m_startAfterHasBeenSet = true; m_startAfter = value; }
    inline void SetStartAfter(Aws::String&& value) { m_startAfterHasBeenSet = true; m_startAfter = std::move(value); }
    inline void SetStartAfter(const char* value) { m_startAfterHasBeenSet = true; m_startAfter.assign(value); }
    inline ListObjectsV2Request& WithStartAfter(const Aws::String& value) { SetStartAfter(value); return *this;}
    inline ListObjectsV2Request& WithStartAfter(Aws::String&& value) { SetStartAfter(std::move(value)); return *this;}
    inline ListObjectsV2Request& WithStartAfter(const char* value) { SetStartAfter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Confirms that the requester knows that she or he will be charged for the list
     * objects request in V2 style. Bucket owners need not specify this parameter in
     * their requests.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const RequestPayer& GetRequestPayer() const{ return m_requestPayer; }
    inline bool RequestPayerHasBeenSet() const { return m_requestPayerHasBeenSet; }
    inline void SetRequestPayer(const RequestPayer& value) { m_requestPayerHasBeenSet = true; m_requestPayer = value; }
    inline void SetRequestPayer(RequestPayer&& value) { m_requestPayerHasBeenSet = true; m_requestPayer = std::move(value); }
    inline ListObjectsV2Request& WithRequestPayer(const RequestPayer& value) { SetRequestPayer(value); return *this;}
    inline ListObjectsV2Request& WithRequestPayer(RequestPayer&& value) { SetRequestPayer(std::move(value)); return *this;}
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
    inline ListObjectsV2Request& WithExpectedBucketOwner(const Aws::String& value) { SetExpectedBucketOwner(value); return *this;}
    inline ListObjectsV2Request& WithExpectedBucketOwner(Aws::String&& value) { SetExpectedBucketOwner(std::move(value)); return *this;}
    inline ListObjectsV2Request& WithExpectedBucketOwner(const char* value) { SetExpectedBucketOwner(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the optional fields that you want returned in the response. Fields
     * that you do not specify are not returned.</p>  <p>This functionality is
     * not supported for directory buckets.</p> 
     */
    inline const Aws::Vector<OptionalObjectAttributes>& GetOptionalObjectAttributes() const{ return m_optionalObjectAttributes; }
    inline bool OptionalObjectAttributesHasBeenSet() const { return m_optionalObjectAttributesHasBeenSet; }
    inline void SetOptionalObjectAttributes(const Aws::Vector<OptionalObjectAttributes>& value) { m_optionalObjectAttributesHasBeenSet = true; m_optionalObjectAttributes = value; }
    inline void SetOptionalObjectAttributes(Aws::Vector<OptionalObjectAttributes>&& value) { m_optionalObjectAttributesHasBeenSet = true; m_optionalObjectAttributes = std::move(value); }
    inline ListObjectsV2Request& WithOptionalObjectAttributes(const Aws::Vector<OptionalObjectAttributes>& value) { SetOptionalObjectAttributes(value); return *this;}
    inline ListObjectsV2Request& WithOptionalObjectAttributes(Aws::Vector<OptionalObjectAttributes>&& value) { SetOptionalObjectAttributes(std::move(value)); return *this;}
    inline ListObjectsV2Request& AddOptionalObjectAttributes(const OptionalObjectAttributes& value) { m_optionalObjectAttributesHasBeenSet = true; m_optionalObjectAttributes.push_back(value); return *this; }
    inline ListObjectsV2Request& AddOptionalObjectAttributes(OptionalObjectAttributes&& value) { m_optionalObjectAttributesHasBeenSet = true; m_optionalObjectAttributes.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline ListObjectsV2Request& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline ListObjectsV2Request& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline ListObjectsV2Request& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline ListObjectsV2Request& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline ListObjectsV2Request& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline ListObjectsV2Request& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline ListObjectsV2Request& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline ListObjectsV2Request& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline ListObjectsV2Request& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Aws::String m_delimiter;
    bool m_delimiterHasBeenSet = false;

    EncodingType m_encodingType;
    bool m_encodingTypeHasBeenSet = false;

    int m_maxKeys;
    bool m_maxKeysHasBeenSet = false;

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    Aws::String m_continuationToken;
    bool m_continuationTokenHasBeenSet = false;

    bool m_fetchOwner;
    bool m_fetchOwnerHasBeenSet = false;

    Aws::String m_startAfter;
    bool m_startAfterHasBeenSet = false;

    RequestPayer m_requestPayer;
    bool m_requestPayerHasBeenSet = false;

    Aws::String m_expectedBucketOwner;
    bool m_expectedBucketOwnerHasBeenSet = false;

    Aws::Vector<OptionalObjectAttributes> m_optionalObjectAttributes;
    bool m_optionalObjectAttributesHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
