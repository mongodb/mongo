/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/CopyPartResult.h>
#include <aws/s3/model/ServerSideEncryption.h>
#include <aws/s3/model/RequestCharged.h>
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
  class UploadPartCopyResult
  {
  public:
    AWS_S3_API UploadPartCopyResult();
    AWS_S3_API UploadPartCopyResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API UploadPartCopyResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The version of the source object that was copied, if you have enabled
     * versioning on the source bucket.</p>  <p>This functionality is not
     * supported when the source object is in a directory bucket.</p> 
     */
    inline const Aws::String& GetCopySourceVersionId() const{ return m_copySourceVersionId; }
    inline void SetCopySourceVersionId(const Aws::String& value) { m_copySourceVersionId = value; }
    inline void SetCopySourceVersionId(Aws::String&& value) { m_copySourceVersionId = std::move(value); }
    inline void SetCopySourceVersionId(const char* value) { m_copySourceVersionId.assign(value); }
    inline UploadPartCopyResult& WithCopySourceVersionId(const Aws::String& value) { SetCopySourceVersionId(value); return *this;}
    inline UploadPartCopyResult& WithCopySourceVersionId(Aws::String&& value) { SetCopySourceVersionId(std::move(value)); return *this;}
    inline UploadPartCopyResult& WithCopySourceVersionId(const char* value) { SetCopySourceVersionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for all response elements.</p>
     */
    inline const CopyPartResult& GetCopyPartResult() const{ return m_copyPartResult; }
    inline void SetCopyPartResult(const CopyPartResult& value) { m_copyPartResult = value; }
    inline void SetCopyPartResult(CopyPartResult&& value) { m_copyPartResult = std::move(value); }
    inline UploadPartCopyResult& WithCopyPartResult(const CopyPartResult& value) { SetCopyPartResult(value); return *this;}
    inline UploadPartCopyResult& WithCopyPartResult(CopyPartResult&& value) { SetCopyPartResult(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The server-side encryption algorithm used when you store this object in
     * Amazon S3 (for example, <code>AES256</code>, <code>aws:kms</code>).</p>
     */
    inline const ServerSideEncryption& GetServerSideEncryption() const{ return m_serverSideEncryption; }
    inline void SetServerSideEncryption(const ServerSideEncryption& value) { m_serverSideEncryption = value; }
    inline void SetServerSideEncryption(ServerSideEncryption&& value) { m_serverSideEncryption = std::move(value); }
    inline UploadPartCopyResult& WithServerSideEncryption(const ServerSideEncryption& value) { SetServerSideEncryption(value); return *this;}
    inline UploadPartCopyResult& WithServerSideEncryption(ServerSideEncryption&& value) { SetServerSideEncryption(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>If server-side encryption with a customer-provided encryption key was
     * requested, the response will include this header to confirm the encryption
     * algorithm that's used.</p>  <p>This functionality is not supported for
     * directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerAlgorithm() const{ return m_sSECustomerAlgorithm; }
    inline void SetSSECustomerAlgorithm(const Aws::String& value) { m_sSECustomerAlgorithm = value; }
    inline void SetSSECustomerAlgorithm(Aws::String&& value) { m_sSECustomerAlgorithm = std::move(value); }
    inline void SetSSECustomerAlgorithm(const char* value) { m_sSECustomerAlgorithm.assign(value); }
    inline UploadPartCopyResult& WithSSECustomerAlgorithm(const Aws::String& value) { SetSSECustomerAlgorithm(value); return *this;}
    inline UploadPartCopyResult& WithSSECustomerAlgorithm(Aws::String&& value) { SetSSECustomerAlgorithm(std::move(value)); return *this;}
    inline UploadPartCopyResult& WithSSECustomerAlgorithm(const char* value) { SetSSECustomerAlgorithm(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If server-side encryption with a customer-provided encryption key was
     * requested, the response will include this header to provide the round-trip
     * message integrity verification of the customer-provided encryption key.</p>
     *  <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerKeyMD5() const{ return m_sSECustomerKeyMD5; }
    inline void SetSSECustomerKeyMD5(const Aws::String& value) { m_sSECustomerKeyMD5 = value; }
    inline void SetSSECustomerKeyMD5(Aws::String&& value) { m_sSECustomerKeyMD5 = std::move(value); }
    inline void SetSSECustomerKeyMD5(const char* value) { m_sSECustomerKeyMD5.assign(value); }
    inline UploadPartCopyResult& WithSSECustomerKeyMD5(const Aws::String& value) { SetSSECustomerKeyMD5(value); return *this;}
    inline UploadPartCopyResult& WithSSECustomerKeyMD5(Aws::String&& value) { SetSSECustomerKeyMD5(std::move(value)); return *this;}
    inline UploadPartCopyResult& WithSSECustomerKeyMD5(const char* value) { SetSSECustomerKeyMD5(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If present, indicates the ID of the KMS key that was used for object
     * encryption.</p>
     */
    inline const Aws::String& GetSSEKMSKeyId() const{ return m_sSEKMSKeyId; }
    inline void SetSSEKMSKeyId(const Aws::String& value) { m_sSEKMSKeyId = value; }
    inline void SetSSEKMSKeyId(Aws::String&& value) { m_sSEKMSKeyId = std::move(value); }
    inline void SetSSEKMSKeyId(const char* value) { m_sSEKMSKeyId.assign(value); }
    inline UploadPartCopyResult& WithSSEKMSKeyId(const Aws::String& value) { SetSSEKMSKeyId(value); return *this;}
    inline UploadPartCopyResult& WithSSEKMSKeyId(Aws::String&& value) { SetSSEKMSKeyId(std::move(value)); return *this;}
    inline UploadPartCopyResult& WithSSEKMSKeyId(const char* value) { SetSSEKMSKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether the multipart upload uses an S3 Bucket Key for server-side
     * encryption with Key Management Service (KMS) keys (SSE-KMS).</p>
     */
    inline bool GetBucketKeyEnabled() const{ return m_bucketKeyEnabled; }
    inline void SetBucketKeyEnabled(bool value) { m_bucketKeyEnabled = value; }
    inline UploadPartCopyResult& WithBucketKeyEnabled(bool value) { SetBucketKeyEnabled(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline UploadPartCopyResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline UploadPartCopyResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline UploadPartCopyResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline UploadPartCopyResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline UploadPartCopyResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_copySourceVersionId;

    CopyPartResult m_copyPartResult;

    ServerSideEncryption m_serverSideEncryption;

    Aws::String m_sSECustomerAlgorithm;

    Aws::String m_sSECustomerKeyMD5;

    Aws::String m_sSEKMSKeyId;

    bool m_bucketKeyEnabled;

    RequestCharged m_requestCharged;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
