/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/ServerSideEncryption.h>
#include <aws/s3/model/RequestCharged.h>
#include <aws/s3/model/ChecksumAlgorithm.h>
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
  class CreateMultipartUploadResult
  {
  public:
    AWS_S3_API CreateMultipartUploadResult();
    AWS_S3_API CreateMultipartUploadResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API CreateMultipartUploadResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>If the bucket has a lifecycle rule configured with an action to abort
     * incomplete multipart uploads and the prefix in the lifecycle rule matches the
     * object name in the request, the response includes this header. The header
     * indicates when the initiated multipart upload becomes eligible for an abort
     * operation. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuoverview.html#mpu-abort-incomplete-mpu-lifecycle-config">
     * Aborting Incomplete Multipart Uploads Using a Bucket Lifecycle Configuration</a>
     * in the <i>Amazon S3 User Guide</i>.</p> <p>The response also includes the
     * <code>x-amz-abort-rule-id</code> header that provides the ID of the lifecycle
     * configuration rule that defines the abort action.</p>  <p>This
     * functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::Utils::DateTime& GetAbortDate() const{ return m_abortDate; }
    inline void SetAbortDate(const Aws::Utils::DateTime& value) { m_abortDate = value; }
    inline void SetAbortDate(Aws::Utils::DateTime&& value) { m_abortDate = std::move(value); }
    inline CreateMultipartUploadResult& WithAbortDate(const Aws::Utils::DateTime& value) { SetAbortDate(value); return *this;}
    inline CreateMultipartUploadResult& WithAbortDate(Aws::Utils::DateTime&& value) { SetAbortDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header is returned along with the <code>x-amz-abort-date</code> header.
     * It identifies the applicable lifecycle configuration rule that defines the
     * action to abort incomplete multipart uploads.</p>  <p>This functionality
     * is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetAbortRuleId() const{ return m_abortRuleId; }
    inline void SetAbortRuleId(const Aws::String& value) { m_abortRuleId = value; }
    inline void SetAbortRuleId(Aws::String&& value) { m_abortRuleId = std::move(value); }
    inline void SetAbortRuleId(const char* value) { m_abortRuleId.assign(value); }
    inline CreateMultipartUploadResult& WithAbortRuleId(const Aws::String& value) { SetAbortRuleId(value); return *this;}
    inline CreateMultipartUploadResult& WithAbortRuleId(Aws::String&& value) { SetAbortRuleId(std::move(value)); return *this;}
    inline CreateMultipartUploadResult& WithAbortRuleId(const char* value) { SetAbortRuleId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the bucket to which the multipart upload was initiated. Does not
     * return the access point ARN or access point alias if used.</p>  <p>Access
     * points are not supported by directory buckets.</p> 
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline void SetBucket(const Aws::String& value) { m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucket.assign(value); }
    inline CreateMultipartUploadResult& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline CreateMultipartUploadResult& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline CreateMultipartUploadResult& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Object key for which the multipart upload was initiated.</p>
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline void SetKey(const Aws::String& value) { m_key = value; }
    inline void SetKey(Aws::String&& value) { m_key = std::move(value); }
    inline void SetKey(const char* value) { m_key.assign(value); }
    inline CreateMultipartUploadResult& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline CreateMultipartUploadResult& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline CreateMultipartUploadResult& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>ID for the initiated multipart upload.</p>
     */
    inline const Aws::String& GetUploadId() const{ return m_uploadId; }
    inline void SetUploadId(const Aws::String& value) { m_uploadId = value; }
    inline void SetUploadId(Aws::String&& value) { m_uploadId = std::move(value); }
    inline void SetUploadId(const char* value) { m_uploadId.assign(value); }
    inline CreateMultipartUploadResult& WithUploadId(const Aws::String& value) { SetUploadId(value); return *this;}
    inline CreateMultipartUploadResult& WithUploadId(Aws::String&& value) { SetUploadId(std::move(value)); return *this;}
    inline CreateMultipartUploadResult& WithUploadId(const char* value) { SetUploadId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The server-side encryption algorithm used when you store this object in
     * Amazon S3 (for example, <code>AES256</code>, <code>aws:kms</code>).</p>
     */
    inline const ServerSideEncryption& GetServerSideEncryption() const{ return m_serverSideEncryption; }
    inline void SetServerSideEncryption(const ServerSideEncryption& value) { m_serverSideEncryption = value; }
    inline void SetServerSideEncryption(ServerSideEncryption&& value) { m_serverSideEncryption = std::move(value); }
    inline CreateMultipartUploadResult& WithServerSideEncryption(const ServerSideEncryption& value) { SetServerSideEncryption(value); return *this;}
    inline CreateMultipartUploadResult& WithServerSideEncryption(ServerSideEncryption&& value) { SetServerSideEncryption(std::move(value)); return *this;}
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
    inline CreateMultipartUploadResult& WithSSECustomerAlgorithm(const Aws::String& value) { SetSSECustomerAlgorithm(value); return *this;}
    inline CreateMultipartUploadResult& WithSSECustomerAlgorithm(Aws::String&& value) { SetSSECustomerAlgorithm(std::move(value)); return *this;}
    inline CreateMultipartUploadResult& WithSSECustomerAlgorithm(const char* value) { SetSSECustomerAlgorithm(value); return *this;}
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
    inline CreateMultipartUploadResult& WithSSECustomerKeyMD5(const Aws::String& value) { SetSSECustomerKeyMD5(value); return *this;}
    inline CreateMultipartUploadResult& WithSSECustomerKeyMD5(Aws::String&& value) { SetSSECustomerKeyMD5(std::move(value)); return *this;}
    inline CreateMultipartUploadResult& WithSSECustomerKeyMD5(const char* value) { SetSSECustomerKeyMD5(value); return *this;}
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
    inline CreateMultipartUploadResult& WithSSEKMSKeyId(const Aws::String& value) { SetSSEKMSKeyId(value); return *this;}
    inline CreateMultipartUploadResult& WithSSEKMSKeyId(Aws::String&& value) { SetSSEKMSKeyId(std::move(value)); return *this;}
    inline CreateMultipartUploadResult& WithSSEKMSKeyId(const char* value) { SetSSEKMSKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If present, indicates the Amazon Web Services KMS Encryption Context to use
     * for object encryption. The value of this header is a Base64-encoded string of a
     * UTF-8 encoded JSON, which contains the encryption context as key-value
     * pairs.</p>
     */
    inline const Aws::String& GetSSEKMSEncryptionContext() const{ return m_sSEKMSEncryptionContext; }
    inline void SetSSEKMSEncryptionContext(const Aws::String& value) { m_sSEKMSEncryptionContext = value; }
    inline void SetSSEKMSEncryptionContext(Aws::String&& value) { m_sSEKMSEncryptionContext = std::move(value); }
    inline void SetSSEKMSEncryptionContext(const char* value) { m_sSEKMSEncryptionContext.assign(value); }
    inline CreateMultipartUploadResult& WithSSEKMSEncryptionContext(const Aws::String& value) { SetSSEKMSEncryptionContext(value); return *this;}
    inline CreateMultipartUploadResult& WithSSEKMSEncryptionContext(Aws::String&& value) { SetSSEKMSEncryptionContext(std::move(value)); return *this;}
    inline CreateMultipartUploadResult& WithSSEKMSEncryptionContext(const char* value) { SetSSEKMSEncryptionContext(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether the multipart upload uses an S3 Bucket Key for server-side
     * encryption with Key Management Service (KMS) keys (SSE-KMS).</p>
     */
    inline bool GetBucketKeyEnabled() const{ return m_bucketKeyEnabled; }
    inline void SetBucketKeyEnabled(bool value) { m_bucketKeyEnabled = value; }
    inline CreateMultipartUploadResult& WithBucketKeyEnabled(bool value) { SetBucketKeyEnabled(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline CreateMultipartUploadResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline CreateMultipartUploadResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The algorithm that was used to create a checksum of the object.</p>
     */
    inline const ChecksumAlgorithm& GetChecksumAlgorithm() const{ return m_checksumAlgorithm; }
    inline void SetChecksumAlgorithm(const ChecksumAlgorithm& value) { m_checksumAlgorithm = value; }
    inline void SetChecksumAlgorithm(ChecksumAlgorithm&& value) { m_checksumAlgorithm = std::move(value); }
    inline CreateMultipartUploadResult& WithChecksumAlgorithm(const ChecksumAlgorithm& value) { SetChecksumAlgorithm(value); return *this;}
    inline CreateMultipartUploadResult& WithChecksumAlgorithm(ChecksumAlgorithm&& value) { SetChecksumAlgorithm(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline CreateMultipartUploadResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline CreateMultipartUploadResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline CreateMultipartUploadResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Utils::DateTime m_abortDate;

    Aws::String m_abortRuleId;

    Aws::String m_bucket;

    Aws::String m_key;

    Aws::String m_uploadId;

    ServerSideEncryption m_serverSideEncryption;

    Aws::String m_sSECustomerAlgorithm;

    Aws::String m_sSECustomerKeyMD5;

    Aws::String m_sSEKMSKeyId;

    Aws::String m_sSEKMSEncryptionContext;

    bool m_bucketKeyEnabled;

    RequestCharged m_requestCharged;

    ChecksumAlgorithm m_checksumAlgorithm;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
