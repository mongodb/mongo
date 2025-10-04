/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
  class ListObjectsV2Result
  {
  public:
    AWS_S3_API ListObjectsV2Result();
    AWS_S3_API ListObjectsV2Result(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API ListObjectsV2Result& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Set to <code>false</code> if all of the results were returned. Set to
     * <code>true</code> if more keys are available to return. If the number of results
     * exceeds that specified by <code>MaxKeys</code>, all of the results might not be
     * returned.</p>
     */
    inline bool GetIsTruncated() const{ return m_isTruncated; }
    inline void SetIsTruncated(bool value) { m_isTruncated = value; }
    inline ListObjectsV2Result& WithIsTruncated(bool value) { SetIsTruncated(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Metadata about each object returned.</p>
     */
    inline const Aws::Vector<Object>& GetContents() const{ return m_contents; }
    inline void SetContents(const Aws::Vector<Object>& value) { m_contents = value; }
    inline void SetContents(Aws::Vector<Object>&& value) { m_contents = std::move(value); }
    inline ListObjectsV2Result& WithContents(const Aws::Vector<Object>& value) { SetContents(value); return *this;}
    inline ListObjectsV2Result& WithContents(Aws::Vector<Object>&& value) { SetContents(std::move(value)); return *this;}
    inline ListObjectsV2Result& AddContents(const Object& value) { m_contents.push_back(value); return *this; }
    inline ListObjectsV2Result& AddContents(Object&& value) { m_contents.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The bucket name.</p>
     */
    inline const Aws::String& GetName() const{ return m_name; }
    inline void SetName(const Aws::String& value) { m_name = value; }
    inline void SetName(Aws::String&& value) { m_name = std::move(value); }
    inline void SetName(const char* value) { m_name.assign(value); }
    inline ListObjectsV2Result& WithName(const Aws::String& value) { SetName(value); return *this;}
    inline ListObjectsV2Result& WithName(Aws::String&& value) { SetName(std::move(value)); return *this;}
    inline ListObjectsV2Result& WithName(const char* value) { SetName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Keys that begin with the indicated prefix.</p>  <p> <b>Directory
     * buckets</b> - For directory buckets, only prefixes that end in a delimiter
     * (<code>/</code>) are supported.</p> 
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline void SetPrefix(const Aws::String& value) { m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefix.assign(value); }
    inline ListObjectsV2Result& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ListObjectsV2Result& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ListObjectsV2Result& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Causes keys that contain the same string between the <code>prefix</code> and
     * the first occurrence of the delimiter to be rolled up into a single result
     * element in the <code>CommonPrefixes</code> collection. These rolled-up keys are
     * not returned elsewhere in the response. Each rolled-up result counts as only one
     * return against the <code>MaxKeys</code> value.</p>  <p> <b>Directory
     * buckets</b> - For directory buckets, <code>/</code> is the only supported
     * delimiter.</p> 
     */
    inline const Aws::String& GetDelimiter() const{ return m_delimiter; }
    inline void SetDelimiter(const Aws::String& value) { m_delimiter = value; }
    inline void SetDelimiter(Aws::String&& value) { m_delimiter = std::move(value); }
    inline void SetDelimiter(const char* value) { m_delimiter.assign(value); }
    inline ListObjectsV2Result& WithDelimiter(const Aws::String& value) { SetDelimiter(value); return *this;}
    inline ListObjectsV2Result& WithDelimiter(Aws::String&& value) { SetDelimiter(std::move(value)); return *this;}
    inline ListObjectsV2Result& WithDelimiter(const char* value) { SetDelimiter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Sets the maximum number of keys returned in the response. By default, the
     * action returns up to 1,000 key names. The response might contain fewer keys but
     * will never contain more.</p>
     */
    inline int GetMaxKeys() const{ return m_maxKeys; }
    inline void SetMaxKeys(int value) { m_maxKeys = value; }
    inline ListObjectsV2Result& WithMaxKeys(int value) { SetMaxKeys(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>All of the keys (up to 1,000) that share the same prefix are grouped
     * together. When counting the total numbers of returns by this API operation, this
     * group of keys is considered as one item.</p> <p>A response can contain
     * <code>CommonPrefixes</code> only if you specify a delimiter.</p> <p>
     * <code>CommonPrefixes</code> contains all (if there are any) keys between
     * <code>Prefix</code> and the next occurrence of the string specified by a
     * delimiter.</p> <p> <code>CommonPrefixes</code> lists keys that act like
     * subdirectories in the directory specified by <code>Prefix</code>.</p> <p>For
     * example, if the prefix is <code>notes/</code> and the delimiter is a slash
     * (<code>/</code>) as in <code>notes/summer/july</code>, the common prefix is
     * <code>notes/summer/</code>. All of the keys that roll up into a common prefix
     * count as a single return when calculating the number of returns. </p> 
     * <ul> <li> <p> <b>Directory buckets</b> - For directory buckets, only prefixes
     * that end in a delimiter (<code>/</code>) are supported.</p> </li> <li> <p>
     * <b>Directory buckets </b> - When you query <code>ListObjectsV2</code> with a
     * delimiter during in-progress multipart uploads, the <code>CommonPrefixes</code>
     * response parameter contains the prefixes that are associated with the
     * in-progress multipart uploads. For more information about multipart uploads, see
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuoverview.html">Multipart
     * Upload Overview</a> in the <i>Amazon S3 User Guide</i>.</p> </li> </ul> 
     */
    inline const Aws::Vector<CommonPrefix>& GetCommonPrefixes() const{ return m_commonPrefixes; }
    inline void SetCommonPrefixes(const Aws::Vector<CommonPrefix>& value) { m_commonPrefixes = value; }
    inline void SetCommonPrefixes(Aws::Vector<CommonPrefix>&& value) { m_commonPrefixes = std::move(value); }
    inline ListObjectsV2Result& WithCommonPrefixes(const Aws::Vector<CommonPrefix>& value) { SetCommonPrefixes(value); return *this;}
    inline ListObjectsV2Result& WithCommonPrefixes(Aws::Vector<CommonPrefix>&& value) { SetCommonPrefixes(std::move(value)); return *this;}
    inline ListObjectsV2Result& AddCommonPrefixes(const CommonPrefix& value) { m_commonPrefixes.push_back(value); return *this; }
    inline ListObjectsV2Result& AddCommonPrefixes(CommonPrefix&& value) { m_commonPrefixes.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Encoding type used by Amazon S3 to encode object key names in the XML
     * response.</p> <p>If you specify the <code>encoding-type</code> request
     * parameter, Amazon S3 includes this element in the response, and returns encoded
     * key name values in the following response elements:</p> <p> <code>Delimiter,
     * Prefix, Key,</code> and <code>StartAfter</code>.</p>
     */
    inline const EncodingType& GetEncodingType() const{ return m_encodingType; }
    inline void SetEncodingType(const EncodingType& value) { m_encodingType = value; }
    inline void SetEncodingType(EncodingType&& value) { m_encodingType = std::move(value); }
    inline ListObjectsV2Result& WithEncodingType(const EncodingType& value) { SetEncodingType(value); return *this;}
    inline ListObjectsV2Result& WithEncodingType(EncodingType&& value) { SetEncodingType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p> <code>KeyCount</code> is the number of keys returned with this request.
     * <code>KeyCount</code> will always be less than or equal to the
     * <code>MaxKeys</code> field. For example, if you ask for 50 keys, your result
     * will include 50 keys or fewer.</p>
     */
    inline int GetKeyCount() const{ return m_keyCount; }
    inline void SetKeyCount(int value) { m_keyCount = value; }
    inline ListObjectsV2Result& WithKeyCount(int value) { SetKeyCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> If <code>ContinuationToken</code> was sent with the request, it is included
     * in the response. You can use the returned <code>ContinuationToken</code> for
     * pagination of the list response. You can use this <code>ContinuationToken</code>
     * for pagination of the list results. </p>
     */
    inline const Aws::String& GetContinuationToken() const{ return m_continuationToken; }
    inline void SetContinuationToken(const Aws::String& value) { m_continuationToken = value; }
    inline void SetContinuationToken(Aws::String&& value) { m_continuationToken = std::move(value); }
    inline void SetContinuationToken(const char* value) { m_continuationToken.assign(value); }
    inline ListObjectsV2Result& WithContinuationToken(const Aws::String& value) { SetContinuationToken(value); return *this;}
    inline ListObjectsV2Result& WithContinuationToken(Aws::String&& value) { SetContinuationToken(std::move(value)); return *this;}
    inline ListObjectsV2Result& WithContinuationToken(const char* value) { SetContinuationToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> <code>NextContinuationToken</code> is sent when <code>isTruncated</code> is
     * true, which means there are more keys in the bucket that can be listed. The next
     * list requests to Amazon S3 can be continued with this
     * <code>NextContinuationToken</code>. <code>NextContinuationToken</code> is
     * obfuscated and is not a real key</p>
     */
    inline const Aws::String& GetNextContinuationToken() const{ return m_nextContinuationToken; }
    inline void SetNextContinuationToken(const Aws::String& value) { m_nextContinuationToken = value; }
    inline void SetNextContinuationToken(Aws::String&& value) { m_nextContinuationToken = std::move(value); }
    inline void SetNextContinuationToken(const char* value) { m_nextContinuationToken.assign(value); }
    inline ListObjectsV2Result& WithNextContinuationToken(const Aws::String& value) { SetNextContinuationToken(value); return *this;}
    inline ListObjectsV2Result& WithNextContinuationToken(Aws::String&& value) { SetNextContinuationToken(std::move(value)); return *this;}
    inline ListObjectsV2Result& WithNextContinuationToken(const char* value) { SetNextContinuationToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If StartAfter was sent with the request, it is included in the response.</p>
     *  <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetStartAfter() const{ return m_startAfter; }
    inline void SetStartAfter(const Aws::String& value) { m_startAfter = value; }
    inline void SetStartAfter(Aws::String&& value) { m_startAfter = std::move(value); }
    inline void SetStartAfter(const char* value) { m_startAfter.assign(value); }
    inline ListObjectsV2Result& WithStartAfter(const Aws::String& value) { SetStartAfter(value); return *this;}
    inline ListObjectsV2Result& WithStartAfter(Aws::String&& value) { SetStartAfter(std::move(value)); return *this;}
    inline ListObjectsV2Result& WithStartAfter(const char* value) { SetStartAfter(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline ListObjectsV2Result& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline ListObjectsV2Result& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListObjectsV2Result& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListObjectsV2Result& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListObjectsV2Result& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    bool m_isTruncated;

    Aws::Vector<Object> m_contents;

    Aws::String m_name;

    Aws::String m_prefix;

    Aws::String m_delimiter;

    int m_maxKeys;

    Aws::Vector<CommonPrefix> m_commonPrefixes;

    EncodingType m_encodingType;

    int m_keyCount;

    Aws::String m_continuationToken;

    Aws::String m_nextContinuationToken;

    Aws::String m_startAfter;

    RequestCharged m_requestCharged;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
