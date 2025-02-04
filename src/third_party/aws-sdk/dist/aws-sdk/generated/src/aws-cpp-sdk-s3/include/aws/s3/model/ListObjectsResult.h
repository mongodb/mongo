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
#include <aws/s3/model/Object.h>
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
  class ListObjectsResult
  {
  public:
    AWS_S3_API ListObjectsResult();
    AWS_S3_API ListObjectsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API ListObjectsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A flag that indicates whether Amazon S3 returned all of the results that
     * satisfied the search criteria.</p>
     */
    inline bool GetIsTruncated() const{ return m_isTruncated; }
    inline void SetIsTruncated(bool value) { m_isTruncated = value; }
    inline ListObjectsResult& WithIsTruncated(bool value) { SetIsTruncated(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates where in the bucket listing begins. Marker is included in the
     * response if it was sent with the request.</p>
     */
    inline const Aws::String& GetMarker() const{ return m_marker; }
    inline void SetMarker(const Aws::String& value) { m_marker = value; }
    inline void SetMarker(Aws::String&& value) { m_marker = std::move(value); }
    inline void SetMarker(const char* value) { m_marker.assign(value); }
    inline ListObjectsResult& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline ListObjectsResult& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline ListObjectsResult& WithMarker(const char* value) { SetMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When the response is truncated (the <code>IsTruncated</code> element value in
     * the response is <code>true</code>), you can use the key name in this field as
     * the <code>marker</code> parameter in the subsequent request to get the next set
     * of objects. Amazon S3 lists objects in alphabetical order. </p>  <p>This
     * element is returned only if you have the <code>delimiter</code> request
     * parameter specified. If the response does not include the
     * <code>NextMarker</code> element and it is truncated, you can use the value of
     * the last <code>Key</code> element in the response as the <code>marker</code>
     * parameter in the subsequent request to get the next set of object keys.</p>
     * 
     */
    inline const Aws::String& GetNextMarker() const{ return m_nextMarker; }
    inline void SetNextMarker(const Aws::String& value) { m_nextMarker = value; }
    inline void SetNextMarker(Aws::String&& value) { m_nextMarker = std::move(value); }
    inline void SetNextMarker(const char* value) { m_nextMarker.assign(value); }
    inline ListObjectsResult& WithNextMarker(const Aws::String& value) { SetNextMarker(value); return *this;}
    inline ListObjectsResult& WithNextMarker(Aws::String&& value) { SetNextMarker(std::move(value)); return *this;}
    inline ListObjectsResult& WithNextMarker(const char* value) { SetNextMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Metadata about each object returned.</p>
     */
    inline const Aws::Vector<Object>& GetContents() const{ return m_contents; }
    inline void SetContents(const Aws::Vector<Object>& value) { m_contents = value; }
    inline void SetContents(Aws::Vector<Object>&& value) { m_contents = std::move(value); }
    inline ListObjectsResult& WithContents(const Aws::Vector<Object>& value) { SetContents(value); return *this;}
    inline ListObjectsResult& WithContents(Aws::Vector<Object>&& value) { SetContents(std::move(value)); return *this;}
    inline ListObjectsResult& AddContents(const Object& value) { m_contents.push_back(value); return *this; }
    inline ListObjectsResult& AddContents(Object&& value) { m_contents.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The bucket name.</p>
     */
    inline const Aws::String& GetName() const{ return m_name; }
    inline void SetName(const Aws::String& value) { m_name = value; }
    inline void SetName(Aws::String&& value) { m_name = std::move(value); }
    inline void SetName(const char* value) { m_name.assign(value); }
    inline ListObjectsResult& WithName(const Aws::String& value) { SetName(value); return *this;}
    inline ListObjectsResult& WithName(Aws::String&& value) { SetName(std::move(value)); return *this;}
    inline ListObjectsResult& WithName(const char* value) { SetName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Keys that begin with the indicated prefix.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline void SetPrefix(const Aws::String& value) { m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefix.assign(value); }
    inline ListObjectsResult& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ListObjectsResult& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ListObjectsResult& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Causes keys that contain the same string between the prefix and the first
     * occurrence of the delimiter to be rolled up into a single result element in the
     * <code>CommonPrefixes</code> collection. These rolled-up keys are not returned
     * elsewhere in the response. Each rolled-up result counts as only one return
     * against the <code>MaxKeys</code> value.</p>
     */
    inline const Aws::String& GetDelimiter() const{ return m_delimiter; }
    inline void SetDelimiter(const Aws::String& value) { m_delimiter = value; }
    inline void SetDelimiter(Aws::String&& value) { m_delimiter = std::move(value); }
    inline void SetDelimiter(const char* value) { m_delimiter.assign(value); }
    inline ListObjectsResult& WithDelimiter(const Aws::String& value) { SetDelimiter(value); return *this;}
    inline ListObjectsResult& WithDelimiter(Aws::String&& value) { SetDelimiter(std::move(value)); return *this;}
    inline ListObjectsResult& WithDelimiter(const char* value) { SetDelimiter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of keys returned in the response body.</p>
     */
    inline int GetMaxKeys() const{ return m_maxKeys; }
    inline void SetMaxKeys(int value) { m_maxKeys = value; }
    inline ListObjectsResult& WithMaxKeys(int value) { SetMaxKeys(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>All of the keys (up to 1,000) rolled up in a common prefix count as a single
     * return when calculating the number of returns. </p> <p>A response can contain
     * <code>CommonPrefixes</code> only if you specify a delimiter.</p> <p>
     * <code>CommonPrefixes</code> contains all (if there are any) keys between
     * <code>Prefix</code> and the next occurrence of the string specified by the
     * delimiter.</p> <p> <code>CommonPrefixes</code> lists keys that act like
     * subdirectories in the directory specified by <code>Prefix</code>.</p> <p>For
     * example, if the prefix is <code>notes/</code> and the delimiter is a slash
     * (<code>/</code>), as in <code>notes/summer/july</code>, the common prefix is
     * <code>notes/summer/</code>. All of the keys that roll up into a common prefix
     * count as a single return when calculating the number of returns.</p>
     */
    inline const Aws::Vector<CommonPrefix>& GetCommonPrefixes() const{ return m_commonPrefixes; }
    inline void SetCommonPrefixes(const Aws::Vector<CommonPrefix>& value) { m_commonPrefixes = value; }
    inline void SetCommonPrefixes(Aws::Vector<CommonPrefix>&& value) { m_commonPrefixes = std::move(value); }
    inline ListObjectsResult& WithCommonPrefixes(const Aws::Vector<CommonPrefix>& value) { SetCommonPrefixes(value); return *this;}
    inline ListObjectsResult& WithCommonPrefixes(Aws::Vector<CommonPrefix>&& value) { SetCommonPrefixes(std::move(value)); return *this;}
    inline ListObjectsResult& AddCommonPrefixes(const CommonPrefix& value) { m_commonPrefixes.push_back(value); return *this; }
    inline ListObjectsResult& AddCommonPrefixes(CommonPrefix&& value) { m_commonPrefixes.push_back(std::move(value)); return *this; }
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
    inline void SetEncodingType(const EncodingType& value) { m_encodingType = value; }
    inline void SetEncodingType(EncodingType&& value) { m_encodingType = std::move(value); }
    inline ListObjectsResult& WithEncodingType(const EncodingType& value) { SetEncodingType(value); return *this;}
    inline ListObjectsResult& WithEncodingType(EncodingType&& value) { SetEncodingType(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline ListObjectsResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline ListObjectsResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListObjectsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListObjectsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListObjectsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    bool m_isTruncated;

    Aws::String m_marker;

    Aws::String m_nextMarker;

    Aws::Vector<Object> m_contents;

    Aws::String m_name;

    Aws::String m_prefix;

    Aws::String m_delimiter;

    int m_maxKeys;

    Aws::Vector<CommonPrefix> m_commonPrefixes;

    EncodingType m_encodingType;

    RequestCharged m_requestCharged;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
