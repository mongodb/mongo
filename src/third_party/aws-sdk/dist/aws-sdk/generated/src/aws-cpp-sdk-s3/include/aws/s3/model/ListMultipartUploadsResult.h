/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/EncodingType.h>
#include <aws/s3/model/RequestCharged.h>
#include <aws/s3/model/MultipartUpload.h>
#include <aws/s3/model/CommonPrefix.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Xml
{
  class XmlDocument;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{
  class ListMultipartUploadsResult
  {
  public:
    AWS_S3_API ListMultipartUploadsResult();
    AWS_S3_API ListMultipartUploadsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API ListMultipartUploadsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The name of the bucket to which the multipart upload was initiated. Does not
     * return the access point ARN or access point alias if used.</p>
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline void SetBucket(const Aws::String& value) { m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucket.assign(value); }
    inline ListMultipartUploadsResult& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline ListMultipartUploadsResult& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The key at or after which the listing began.</p>
     */
    inline const Aws::String& GetKeyMarker() const{ return m_keyMarker; }
    inline void SetKeyMarker(const Aws::String& value) { m_keyMarker = value; }
    inline void SetKeyMarker(Aws::String&& value) { m_keyMarker = std::move(value); }
    inline void SetKeyMarker(const char* value) { m_keyMarker.assign(value); }
    inline ListMultipartUploadsResult& WithKeyMarker(const Aws::String& value) { SetKeyMarker(value); return *this;}
    inline ListMultipartUploadsResult& WithKeyMarker(Aws::String&& value) { SetKeyMarker(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& WithKeyMarker(const char* value) { SetKeyMarker(value); return *this;}
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
    inline void SetUploadIdMarker(const Aws::String& value) { m_uploadIdMarker = value; }
    inline void SetUploadIdMarker(Aws::String&& value) { m_uploadIdMarker = std::move(value); }
    inline void SetUploadIdMarker(const char* value) { m_uploadIdMarker.assign(value); }
    inline ListMultipartUploadsResult& WithUploadIdMarker(const Aws::String& value) { SetUploadIdMarker(value); return *this;}
    inline ListMultipartUploadsResult& WithUploadIdMarker(Aws::String&& value) { SetUploadIdMarker(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& WithUploadIdMarker(const char* value) { SetUploadIdMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When a list is truncated, this element specifies the value that should be
     * used for the key-marker request parameter in a subsequent request.</p>
     */
    inline const Aws::String& GetNextKeyMarker() const{ return m_nextKeyMarker; }
    inline void SetNextKeyMarker(const Aws::String& value) { m_nextKeyMarker = value; }
    inline void SetNextKeyMarker(Aws::String&& value) { m_nextKeyMarker = std::move(value); }
    inline void SetNextKeyMarker(const char* value) { m_nextKeyMarker.assign(value); }
    inline ListMultipartUploadsResult& WithNextKeyMarker(const Aws::String& value) { SetNextKeyMarker(value); return *this;}
    inline ListMultipartUploadsResult& WithNextKeyMarker(Aws::String&& value) { SetNextKeyMarker(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& WithNextKeyMarker(const char* value) { SetNextKeyMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When a prefix is provided in the request, this field contains the specified
     * prefix. The result contains only keys starting with the specified prefix.</p>
     *  <p> <b>Directory buckets</b> - For directory buckets, only prefixes that
     * end in a delimiter (<code>/</code>) are supported.</p> 
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline void SetPrefix(const Aws::String& value) { m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefix.assign(value); }
    inline ListMultipartUploadsResult& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ListMultipartUploadsResult& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Contains the delimiter you specified in the request. If you don't specify a
     * delimiter in your request, this element is absent from the response.</p> 
     * <p> <b>Directory buckets</b> - For directory buckets, <code>/</code> is the only
     * supported delimiter.</p> 
     */
    inline const Aws::String& GetDelimiter() const{ return m_delimiter; }
    inline void SetDelimiter(const Aws::String& value) { m_delimiter = value; }
    inline void SetDelimiter(Aws::String&& value) { m_delimiter = std::move(value); }
    inline void SetDelimiter(const char* value) { m_delimiter.assign(value); }
    inline ListMultipartUploadsResult& WithDelimiter(const Aws::String& value) { SetDelimiter(value); return *this;}
    inline ListMultipartUploadsResult& WithDelimiter(Aws::String&& value) { SetDelimiter(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& WithDelimiter(const char* value) { SetDelimiter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When a list is truncated, this element specifies the value that should be
     * used for the <code>upload-id-marker</code> request parameter in a subsequent
     * request.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const Aws::String& GetNextUploadIdMarker() const{ return m_nextUploadIdMarker; }
    inline void SetNextUploadIdMarker(const Aws::String& value) { m_nextUploadIdMarker = value; }
    inline void SetNextUploadIdMarker(Aws::String&& value) { m_nextUploadIdMarker = std::move(value); }
    inline void SetNextUploadIdMarker(const char* value) { m_nextUploadIdMarker.assign(value); }
    inline ListMultipartUploadsResult& WithNextUploadIdMarker(const Aws::String& value) { SetNextUploadIdMarker(value); return *this;}
    inline ListMultipartUploadsResult& WithNextUploadIdMarker(Aws::String&& value) { SetNextUploadIdMarker(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& WithNextUploadIdMarker(const char* value) { SetNextUploadIdMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Maximum number of multipart uploads that could have been included in the
     * response.</p>
     */
    inline int GetMaxUploads() const{ return m_maxUploads; }
    inline void SetMaxUploads(int value) { m_maxUploads = value; }
    inline ListMultipartUploadsResult& WithMaxUploads(int value) { SetMaxUploads(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether the returned list of multipart uploads is truncated. A
     * value of true indicates that the list was truncated. The list can be truncated
     * if the number of multipart uploads exceeds the limit allowed or specified by max
     * uploads.</p>
     */
    inline bool GetIsTruncated() const{ return m_isTruncated; }
    inline void SetIsTruncated(bool value) { m_isTruncated = value; }
    inline ListMultipartUploadsResult& WithIsTruncated(bool value) { SetIsTruncated(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for elements related to a particular multipart upload. A response
     * can contain zero or more <code>Upload</code> elements.</p>
     */
    inline const Aws::Vector<MultipartUpload>& GetUploads() const{ return m_uploads; }
    inline void SetUploads(const Aws::Vector<MultipartUpload>& value) { m_uploads = value; }
    inline void SetUploads(Aws::Vector<MultipartUpload>&& value) { m_uploads = std::move(value); }
    inline ListMultipartUploadsResult& WithUploads(const Aws::Vector<MultipartUpload>& value) { SetUploads(value); return *this;}
    inline ListMultipartUploadsResult& WithUploads(Aws::Vector<MultipartUpload>&& value) { SetUploads(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& AddUploads(const MultipartUpload& value) { m_uploads.push_back(value); return *this; }
    inline ListMultipartUploadsResult& AddUploads(MultipartUpload&& value) { m_uploads.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>If you specify a delimiter in the request, then the result returns each
     * distinct key prefix containing the delimiter in a <code>CommonPrefixes</code>
     * element. The distinct key prefixes are returned in the <code>Prefix</code> child
     * element.</p>  <p> <b>Directory buckets</b> - For directory buckets, only
     * prefixes that end in a delimiter (<code>/</code>) are supported.</p> 
     */
    inline const Aws::Vector<CommonPrefix>& GetCommonPrefixes() const{ return m_commonPrefixes; }
    inline void SetCommonPrefixes(const Aws::Vector<CommonPrefix>& value) { m_commonPrefixes = value; }
    inline void SetCommonPrefixes(Aws::Vector<CommonPrefix>&& value) { m_commonPrefixes = std::move(value); }
    inline ListMultipartUploadsResult& WithCommonPrefixes(const Aws::Vector<CommonPrefix>& value) { SetCommonPrefixes(value); return *this;}
    inline ListMultipartUploadsResult& WithCommonPrefixes(Aws::Vector<CommonPrefix>&& value) { SetCommonPrefixes(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& AddCommonPrefixes(const CommonPrefix& value) { m_commonPrefixes.push_back(value); return *this; }
    inline ListMultipartUploadsResult& AddCommonPrefixes(CommonPrefix&& value) { m_commonPrefixes.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Encoding type used by Amazon S3 to encode object keys in the response.</p>
     * <p>If you specify the <code>encoding-type</code> request parameter, Amazon S3
     * includes this element in the response, and returns encoded key name values in
     * the following response elements:</p> <p> <code>Delimiter</code>,
     * <code>KeyMarker</code>, <code>Prefix</code>, <code>NextKeyMarker</code>,
     * <code>Key</code>.</p>
     */
    inline const EncodingType& GetEncodingType() const{ return m_encodingType; }
    inline void SetEncodingType(const EncodingType& value) { m_encodingType = value; }
    inline void SetEncodingType(EncodingType&& value) { m_encodingType = std::move(value); }
    inline ListMultipartUploadsResult& WithEncodingType(const EncodingType& value) { SetEncodingType(value); return *this;}
    inline ListMultipartUploadsResult& WithEncodingType(EncodingType&& value) { SetEncodingType(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline ListMultipartUploadsResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline ListMultipartUploadsResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListMultipartUploadsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListMultipartUploadsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListMultipartUploadsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_bucket;

    Aws::String m_keyMarker;

    Aws::String m_uploadIdMarker;

    Aws::String m_nextKeyMarker;

    Aws::String m_prefix;

    Aws::String m_delimiter;

    Aws::String m_nextUploadIdMarker;

    int m_maxUploads;

    bool m_isTruncated;

    Aws::Vector<MultipartUpload> m_uploads;

    Aws::Vector<CommonPrefix> m_commonPrefixes;

    EncodingType m_encodingType;

    RequestCharged m_requestCharged;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
