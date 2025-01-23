/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
  class PutObjectResult
  {
  public:
    AWS_S3_API PutObjectResult();
    AWS_S3_API PutObjectResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API PutObjectResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>If the expiration is configured for the object (see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycleConfiguration.html">PutBucketLifecycleConfiguration</a>)
     * in the <i>Amazon S3 User Guide</i>, the response includes this header. It
     * includes the <code>expiry-date</code> and <code>rule-id</code> key-value pairs
     * that provide information about object expiration. The value of the
     * <code>rule-id</code> is URL-encoded.</p>  <p>Object expiration information
     * is not returned in directory buckets and this header returns the value
     * "<code>NotImplemented</code>" in all responses for directory buckets.</p>
     * 
     */
    inline const Aws::String& GetExpiration() const{ return m_expiration; }
    inline void SetExpiration(const Aws::String& value) { m_expiration = value; }
    inline void SetExpiration(Aws::String&& value) { m_expiration = std::move(value); }
    inline void SetExpiration(const char* value) { m_expiration.assign(value); }
    inline PutObjectResult& WithExpiration(const Aws::String& value) { SetExpiration(value); return *this;}
    inline PutObjectResult& WithExpiration(Aws::String&& value) { SetExpiration(std::move(value)); return *this;}
    inline PutObjectResult& WithExpiration(const char* value) { SetExpiration(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Entity tag for the uploaded object.</p> <p> <b>General purpose buckets </b> -
     * To ensure that data is not corrupted traversing the network, for objects where
     * the ETag is the MD5 digest of the object, you can calculate the MD5 while
     * putting an object to Amazon S3 and compare the returned ETag to the calculated
     * MD5 value.</p> <p> <b>Directory buckets </b> - The ETag for the object in a
     * directory bucket isn't the MD5 digest of the object.</p>
     */
    inline const Aws::String& GetETag() const{ return m_eTag; }
    inline void SetETag(const Aws::String& value) { m_eTag = value; }
    inline void SetETag(Aws::String&& value) { m_eTag = std::move(value); }
    inline void SetETag(const char* value) { m_eTag.assign(value); }
    inline PutObjectResult& WithETag(const Aws::String& value) { SetETag(value); return *this;}
    inline PutObjectResult& WithETag(Aws::String&& value) { SetETag(std::move(value)); return *this;}
    inline PutObjectResult& WithETag(const char* value) { SetETag(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 32-bit CRC-32 checksum of the object. This will only be
     * present if it was uploaded with the object. When you use an API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32() const{ return m_checksumCRC32; }
    inline void SetChecksumCRC32(const Aws::String& value) { m_checksumCRC32 = value; }
    inline void SetChecksumCRC32(Aws::String&& value) { m_checksumCRC32 = std::move(value); }
    inline void SetChecksumCRC32(const char* value) { m_checksumCRC32.assign(value); }
    inline PutObjectResult& WithChecksumCRC32(const Aws::String& value) { SetChecksumCRC32(value); return *this;}
    inline PutObjectResult& WithChecksumCRC32(Aws::String&& value) { SetChecksumCRC32(std::move(value)); return *this;}
    inline PutObjectResult& WithChecksumCRC32(const char* value) { SetChecksumCRC32(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 32-bit CRC-32C checksum of the object. This will only be
     * present if it was uploaded with the object. When you use an API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32C() const{ return m_checksumCRC32C; }
    inline void SetChecksumCRC32C(const Aws::String& value) { m_checksumCRC32C = value; }
    inline void SetChecksumCRC32C(Aws::String&& value) { m_checksumCRC32C = std::move(value); }
    inline void SetChecksumCRC32C(const char* value) { m_checksumCRC32C.assign(value); }
    inline PutObjectResult& WithChecksumCRC32C(const Aws::String& value) { SetChecksumCRC32C(value); return *this;}
    inline PutObjectResult& WithChecksumCRC32C(Aws::String&& value) { SetChecksumCRC32C(std::move(value)); return *this;}
    inline PutObjectResult& WithChecksumCRC32C(const char* value) { SetChecksumCRC32C(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 160-bit SHA-1 digest of the object. This will only be
     * present if it was uploaded with the object. When you use the API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA1() const{ return m_checksumSHA1; }
    inline void SetChecksumSHA1(const Aws::String& value) { m_checksumSHA1 = value; }
    inline void SetChecksumSHA1(Aws::String&& value) { m_checksumSHA1 = std::move(value); }
    inline void SetChecksumSHA1(const char* value) { m_checksumSHA1.assign(value); }
    inline PutObjectResult& WithChecksumSHA1(const Aws::String& value) { SetChecksumSHA1(value); return *this;}
    inline PutObjectResult& WithChecksumSHA1(Aws::String&& value) { SetChecksumSHA1(std::move(value)); return *this;}
    inline PutObjectResult& WithChecksumSHA1(const char* value) { SetChecksumSHA1(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 256-bit SHA-256 digest of the object. This will only be
     * present if it was uploaded with the object. When you use an API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA256() const{ return m_checksumSHA256; }
    inline void SetChecksumSHA256(const Aws::String& value) { m_checksumSHA256 = value; }
    inline void SetChecksumSHA256(Aws::String&& value) { m_checksumSHA256 = std::move(value); }
    inline void SetChecksumSHA256(const char* value) { m_checksumSHA256.assign(value); }
    inline PutObjectResult& WithChecksumSHA256(const Aws::String& value) { SetChecksumSHA256(value); return *this;}
    inline PutObjectResult& WithChecksumSHA256(Aws::String&& value) { SetChecksumSHA256(std::move(value)); return *this;}
    inline PutObjectResult& WithChecksumSHA256(const char* value) { SetChecksumSHA256(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The server-side encryption algorithm used when you store this object in
     * Amazon S3.</p>
     */
    inline const ServerSideEncryption& GetServerSideEncryption() const{ return m_serverSideEncryption; }
    inline void SetServerSideEncryption(const ServerSideEncryption& value) { m_serverSideEncryption = value; }
    inline void SetServerSideEncryption(ServerSideEncryption&& value) { m_serverSideEncryption = std::move(value); }
    inline PutObjectResult& WithServerSideEncryption(const ServerSideEncryption& value) { SetServerSideEncryption(value); return *this;}
    inline PutObjectResult& WithServerSideEncryption(ServerSideEncryption&& value) { SetServerSideEncryption(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Version ID of the object.</p> <p>If you enable versioning for a bucket,
     * Amazon S3 automatically generates a unique version ID for the object being
     * stored. Amazon S3 returns this ID in the response. When you enable versioning
     * for a bucket, if Amazon S3 receives multiple write requests for the same object
     * simultaneously, it stores all of the objects. For more information about
     * versioning, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/AddingObjectstoVersioningEnabledBuckets.html">Adding
     * Objects to Versioning-Enabled Buckets</a> in the <i>Amazon S3 User Guide</i>.
     * For information about returning the versioning state of a bucket, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketVersioning.html">GetBucketVersioning</a>.
     * </p>  <p>This functionality is not supported for directory buckets.</p>
     * 
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline void SetVersionId(const Aws::String& value) { m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionId.assign(value); }
    inline PutObjectResult& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline PutObjectResult& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline PutObjectResult& WithVersionId(const char* value) { SetVersionId(value); return *this;}
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
    inline PutObjectResult& WithSSECustomerAlgorithm(const Aws::String& value) { SetSSECustomerAlgorithm(value); return *this;}
    inline PutObjectResult& WithSSECustomerAlgorithm(Aws::String&& value) { SetSSECustomerAlgorithm(std::move(value)); return *this;}
    inline PutObjectResult& WithSSECustomerAlgorithm(const char* value) { SetSSECustomerAlgorithm(value); return *this;}
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
    inline PutObjectResult& WithSSECustomerKeyMD5(const Aws::String& value) { SetSSECustomerKeyMD5(value); return *this;}
    inline PutObjectResult& WithSSECustomerKeyMD5(Aws::String&& value) { SetSSECustomerKeyMD5(std::move(value)); return *this;}
    inline PutObjectResult& WithSSECustomerKeyMD5(const char* value) { SetSSECustomerKeyMD5(value); return *this;}
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
    inline PutObjectResult& WithSSEKMSKeyId(const Aws::String& value) { SetSSEKMSKeyId(value); return *this;}
    inline PutObjectResult& WithSSEKMSKeyId(Aws::String&& value) { SetSSEKMSKeyId(std::move(value)); return *this;}
    inline PutObjectResult& WithSSEKMSKeyId(const char* value) { SetSSEKMSKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If present, indicates the Amazon Web Services KMS Encryption Context to use
     * for object encryption. The value of this header is a Base64-encoded string of a
     * UTF-8 encoded JSON, which contains the encryption context as key-value pairs.
     * This value is stored as object metadata and automatically gets passed on to
     * Amazon Web Services KMS for future <code>GetObject</code> operations on this
     * object.</p>
     */
    inline const Aws::String& GetSSEKMSEncryptionContext() const{ return m_sSEKMSEncryptionContext; }
    inline void SetSSEKMSEncryptionContext(const Aws::String& value) { m_sSEKMSEncryptionContext = value; }
    inline void SetSSEKMSEncryptionContext(Aws::String&& value) { m_sSEKMSEncryptionContext = std::move(value); }
    inline void SetSSEKMSEncryptionContext(const char* value) { m_sSEKMSEncryptionContext.assign(value); }
    inline PutObjectResult& WithSSEKMSEncryptionContext(const Aws::String& value) { SetSSEKMSEncryptionContext(value); return *this;}
    inline PutObjectResult& WithSSEKMSEncryptionContext(Aws::String&& value) { SetSSEKMSEncryptionContext(std::move(value)); return *this;}
    inline PutObjectResult& WithSSEKMSEncryptionContext(const char* value) { SetSSEKMSEncryptionContext(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether the uploaded object uses an S3 Bucket Key for server-side
     * encryption with Key Management Service (KMS) keys (SSE-KMS).</p>
     */
    inline bool GetBucketKeyEnabled() const{ return m_bucketKeyEnabled; }
    inline void SetBucketKeyEnabled(bool value) { m_bucketKeyEnabled = value; }
    inline PutObjectResult& WithBucketKeyEnabled(bool value) { SetBucketKeyEnabled(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The size of the object in bytes. This will only be present if you append to
     * an object. </p>  <p>This functionality is only supported for objects in
     * the Amazon S3 Express One Zone storage class in directory buckets.</p> 
     */
    inline long long GetSize() const{ return m_size; }
    inline void SetSize(long long value) { m_size = value; }
    inline PutObjectResult& WithSize(long long value) { SetSize(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline PutObjectResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline PutObjectResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline PutObjectResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline PutObjectResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline PutObjectResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_expiration;

    Aws::String m_eTag;

    Aws::String m_checksumCRC32;

    Aws::String m_checksumCRC32C;

    Aws::String m_checksumSHA1;

    Aws::String m_checksumSHA256;

    ServerSideEncryption m_serverSideEncryption;

    Aws::String m_versionId;

    Aws::String m_sSECustomerAlgorithm;

    Aws::String m_sSECustomerKeyMD5;

    Aws::String m_sSEKMSKeyId;

    Aws::String m_sSEKMSEncryptionContext;

    bool m_bucketKeyEnabled;

    long long m_size;

    RequestCharged m_requestCharged;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
