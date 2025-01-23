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
#include <aws/s3/model/ObjectVersion.h>
#include <aws/s3/model/DeleteMarkerEntry.h>
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
  class ListObjectVersionsResult
  {
  public:
    AWS_S3_API ListObjectVersionsResult();
    AWS_S3_API ListObjectVersionsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API ListObjectVersionsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A flag that indicates whether Amazon S3 returned all of the results that
     * satisfied the search criteria. If your results were truncated, you can make a
     * follow-up paginated request by using the <code>NextKeyMarker</code> and
     * <code>NextVersionIdMarker</code> response parameters as a starting place in
     * another request to return the rest of the results.</p>
     */
    inline bool GetIsTruncated() const{ return m_isTruncated; }
    inline void SetIsTruncated(bool value) { m_isTruncated = value; }
    inline ListObjectVersionsResult& WithIsTruncated(bool value) { SetIsTruncated(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Marks the last key returned in a truncated response.</p>
     */
    inline const Aws::String& GetKeyMarker() const{ return m_keyMarker; }
    inline void SetKeyMarker(const Aws::String& value) { m_keyMarker = value; }
    inline void SetKeyMarker(Aws::String&& value) { m_keyMarker = std::move(value); }
    inline void SetKeyMarker(const char* value) { m_keyMarker.assign(value); }
    inline ListObjectVersionsResult& WithKeyMarker(const Aws::String& value) { SetKeyMarker(value); return *this;}
    inline ListObjectVersionsResult& WithKeyMarker(Aws::String&& value) { SetKeyMarker(std::move(value)); return *this;}
    inline ListObjectVersionsResult& WithKeyMarker(const char* value) { SetKeyMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Marks the last version of the key returned in a truncated response.</p>
     */
    inline const Aws::String& GetVersionIdMarker() const{ return m_versionIdMarker; }
    inline void SetVersionIdMarker(const Aws::String& value) { m_versionIdMarker = value; }
    inline void SetVersionIdMarker(Aws::String&& value) { m_versionIdMarker = std::move(value); }
    inline void SetVersionIdMarker(const char* value) { m_versionIdMarker.assign(value); }
    inline ListObjectVersionsResult& WithVersionIdMarker(const Aws::String& value) { SetVersionIdMarker(value); return *this;}
    inline ListObjectVersionsResult& WithVersionIdMarker(Aws::String&& value) { SetVersionIdMarker(std::move(value)); return *this;}
    inline ListObjectVersionsResult& WithVersionIdMarker(const char* value) { SetVersionIdMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When the number of responses exceeds the value of <code>MaxKeys</code>,
     * <code>NextKeyMarker</code> specifies the first key not returned that satisfies
     * the search criteria. Use this value for the key-marker request parameter in a
     * subsequent request.</p>
     */
    inline const Aws::String& GetNextKeyMarker() const{ return m_nextKeyMarker; }
    inline void SetNextKeyMarker(const Aws::String& value) { m_nextKeyMarker = value; }
    inline void SetNextKeyMarker(Aws::String&& value) { m_nextKeyMarker = std::move(value); }
    inline void SetNextKeyMarker(const char* value) { m_nextKeyMarker.assign(value); }
    inline ListObjectVersionsResult& WithNextKeyMarker(const Aws::String& value) { SetNextKeyMarker(value); return *this;}
    inline ListObjectVersionsResult& WithNextKeyMarker(Aws::String&& value) { SetNextKeyMarker(std::move(value)); return *this;}
    inline ListObjectVersionsResult& WithNextKeyMarker(const char* value) { SetNextKeyMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When the number of responses exceeds the value of <code>MaxKeys</code>,
     * <code>NextVersionIdMarker</code> specifies the first object version not returned
     * that satisfies the search criteria. Use this value for the
     * <code>version-id-marker</code> request parameter in a subsequent request.</p>
     */
    inline const Aws::String& GetNextVersionIdMarker() const{ return m_nextVersionIdMarker; }
    inline void SetNextVersionIdMarker(const Aws::String& value) { m_nextVersionIdMarker = value; }
    inline void SetNextVersionIdMarker(Aws::String&& value) { m_nextVersionIdMarker = std::move(value); }
    inline void SetNextVersionIdMarker(const char* value) { m_nextVersionIdMarker.assign(value); }
    inline ListObjectVersionsResult& WithNextVersionIdMarker(const Aws::String& value) { SetNextVersionIdMarker(value); return *this;}
    inline ListObjectVersionsResult& WithNextVersionIdMarker(Aws::String&& value) { SetNextVersionIdMarker(std::move(value)); return *this;}
    inline ListObjectVersionsResult& WithNextVersionIdMarker(const char* value) { SetNextVersionIdMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for version information.</p>
     */
    inline const Aws::Vector<ObjectVersion>& GetVersions() const{ return m_versions; }
    inline void SetVersions(const Aws::Vector<ObjectVersion>& value) { m_versions = value; }
    inline void SetVersions(Aws::Vector<ObjectVersion>&& value) { m_versions = std::move(value); }
    inline ListObjectVersionsResult& WithVersions(const Aws::Vector<ObjectVersion>& value) { SetVersions(value); return *this;}
    inline ListObjectVersionsResult& WithVersions(Aws::Vector<ObjectVersion>&& value) { SetVersions(std::move(value)); return *this;}
    inline ListObjectVersionsResult& AddVersions(const ObjectVersion& value) { m_versions.push_back(value); return *this; }
    inline ListObjectVersionsResult& AddVersions(ObjectVersion&& value) { m_versions.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Container for an object that is a delete marker.</p>
     */
    inline const Aws::Vector<DeleteMarkerEntry>& GetDeleteMarkers() const{ return m_deleteMarkers; }
    inline void SetDeleteMarkers(const Aws::Vector<DeleteMarkerEntry>& value) { m_deleteMarkers = value; }
    inline void SetDeleteMarkers(Aws::Vector<DeleteMarkerEntry>&& value) { m_deleteMarkers = std::move(value); }
    inline ListObjectVersionsResult& WithDeleteMarkers(const Aws::Vector<DeleteMarkerEntry>& value) { SetDeleteMarkers(value); return *this;}
    inline ListObjectVersionsResult& WithDeleteMarkers(Aws::Vector<DeleteMarkerEntry>&& value) { SetDeleteMarkers(std::move(value)); return *this;}
    inline ListObjectVersionsResult& AddDeleteMarkers(const DeleteMarkerEntry& value) { m_deleteMarkers.push_back(value); return *this; }
    inline ListObjectVersionsResult& AddDeleteMarkers(DeleteMarkerEntry&& value) { m_deleteMarkers.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The bucket name.</p>
     */
    inline const Aws::String& GetName() const{ return m_name; }
    inline void SetName(const Aws::String& value) { m_name = value; }
    inline void SetName(Aws::String&& value) { m_name = std::move(value); }
    inline void SetName(const char* value) { m_name.assign(value); }
    inline ListObjectVersionsResult& WithName(const Aws::String& value) { SetName(value); return *this;}
    inline ListObjectVersionsResult& WithName(Aws::String&& value) { SetName(std::move(value)); return *this;}
    inline ListObjectVersionsResult& WithName(const char* value) { SetName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Selects objects that start with the value supplied by this parameter.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline void SetPrefix(const Aws::String& value) { m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefix.assign(value); }
    inline ListObjectVersionsResult& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ListObjectVersionsResult& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ListObjectVersionsResult& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The delimiter grouping the included keys. A delimiter is a character that you
     * specify to group keys. All keys that contain the same string between the prefix
     * and the first occurrence of the delimiter are grouped under a single result
     * element in <code>CommonPrefixes</code>. These groups are counted as one result
     * against the <code>max-keys</code> limitation. These keys are not returned
     * elsewhere in the response.</p>
     */
    inline const Aws::String& GetDelimiter() const{ return m_delimiter; }
    inline void SetDelimiter(const Aws::String& value) { m_delimiter = value; }
    inline void SetDelimiter(Aws::String&& value) { m_delimiter = std::move(value); }
    inline void SetDelimiter(const char* value) { m_delimiter.assign(value); }
    inline ListObjectVersionsResult& WithDelimiter(const Aws::String& value) { SetDelimiter(value); return *this;}
    inline ListObjectVersionsResult& WithDelimiter(Aws::String&& value) { SetDelimiter(std::move(value)); return *this;}
    inline ListObjectVersionsResult& WithDelimiter(const char* value) { SetDelimiter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the maximum number of objects to return.</p>
     */
    inline int GetMaxKeys() const{ return m_maxKeys; }
    inline void SetMaxKeys(int value) { m_maxKeys = value; }
    inline ListObjectVersionsResult& WithMaxKeys(int value) { SetMaxKeys(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>All of the keys rolled up into a common prefix count as a single return when
     * calculating the number of returns.</p>
     */
    inline const Aws::Vector<CommonPrefix>& GetCommonPrefixes() const{ return m_commonPrefixes; }
    inline void SetCommonPrefixes(const Aws::Vector<CommonPrefix>& value) { m_commonPrefixes = value; }
    inline void SetCommonPrefixes(Aws::Vector<CommonPrefix>&& value) { m_commonPrefixes = std::move(value); }
    inline ListObjectVersionsResult& WithCommonPrefixes(const Aws::Vector<CommonPrefix>& value) { SetCommonPrefixes(value); return *this;}
    inline ListObjectVersionsResult& WithCommonPrefixes(Aws::Vector<CommonPrefix>&& value) { SetCommonPrefixes(std::move(value)); return *this;}
    inline ListObjectVersionsResult& AddCommonPrefixes(const CommonPrefix& value) { m_commonPrefixes.push_back(value); return *this; }
    inline ListObjectVersionsResult& AddCommonPrefixes(CommonPrefix&& value) { m_commonPrefixes.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p> Encoding type used by Amazon S3 to encode object key names in the XML
     * response.</p> <p>If you specify the <code>encoding-type</code> request
     * parameter, Amazon S3 includes this element in the response, and returns encoded
     * key name values in the following response elements:</p> <p> <code>KeyMarker,
     * NextKeyMarker, Prefix, Key</code>, and <code>Delimiter</code>.</p>
     */
    inline const EncodingType& GetEncodingType() const{ return m_encodingType; }
    inline void SetEncodingType(const EncodingType& value) { m_encodingType = value; }
    inline void SetEncodingType(EncodingType&& value) { m_encodingType = std::move(value); }
    inline ListObjectVersionsResult& WithEncodingType(const EncodingType& value) { SetEncodingType(value); return *this;}
    inline ListObjectVersionsResult& WithEncodingType(EncodingType&& value) { SetEncodingType(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline ListObjectVersionsResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline ListObjectVersionsResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListObjectVersionsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListObjectVersionsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListObjectVersionsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    bool m_isTruncated;

    Aws::String m_keyMarker;

    Aws::String m_versionIdMarker;

    Aws::String m_nextKeyMarker;

    Aws::String m_nextVersionIdMarker;

    Aws::Vector<ObjectVersion> m_versions;

    Aws::Vector<DeleteMarkerEntry> m_deleteMarkers;

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
