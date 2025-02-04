/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ServerSideEncryption.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/SessionCredentials.h>
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
  class CreateSessionResult
  {
  public:
    AWS_S3_API CreateSessionResult();
    AWS_S3_API CreateSessionResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API CreateSessionResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The server-side encryption algorithm used when you store objects in the
     * directory bucket.</p>
     */
    inline const ServerSideEncryption& GetServerSideEncryption() const{ return m_serverSideEncryption; }
    inline void SetServerSideEncryption(const ServerSideEncryption& value) { m_serverSideEncryption = value; }
    inline void SetServerSideEncryption(ServerSideEncryption&& value) { m_serverSideEncryption = std::move(value); }
    inline CreateSessionResult& WithServerSideEncryption(const ServerSideEncryption& value) { SetServerSideEncryption(value); return *this;}
    inline CreateSessionResult& WithServerSideEncryption(ServerSideEncryption&& value) { SetServerSideEncryption(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>If you specify <code>x-amz-server-side-encryption</code> with
     * <code>aws:kms</code>, this header indicates the ID of the KMS symmetric
     * encryption customer managed key that was used for object encryption.</p>
     */
    inline const Aws::String& GetSSEKMSKeyId() const{ return m_sSEKMSKeyId; }
    inline void SetSSEKMSKeyId(const Aws::String& value) { m_sSEKMSKeyId = value; }
    inline void SetSSEKMSKeyId(Aws::String&& value) { m_sSEKMSKeyId = std::move(value); }
    inline void SetSSEKMSKeyId(const char* value) { m_sSEKMSKeyId.assign(value); }
    inline CreateSessionResult& WithSSEKMSKeyId(const Aws::String& value) { SetSSEKMSKeyId(value); return *this;}
    inline CreateSessionResult& WithSSEKMSKeyId(Aws::String&& value) { SetSSEKMSKeyId(std::move(value)); return *this;}
    inline CreateSessionResult& WithSSEKMSKeyId(const char* value) { SetSSEKMSKeyId(value); return *this;}
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
    inline CreateSessionResult& WithSSEKMSEncryptionContext(const Aws::String& value) { SetSSEKMSEncryptionContext(value); return *this;}
    inline CreateSessionResult& WithSSEKMSEncryptionContext(Aws::String&& value) { SetSSEKMSEncryptionContext(std::move(value)); return *this;}
    inline CreateSessionResult& WithSSEKMSEncryptionContext(const char* value) { SetSSEKMSEncryptionContext(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether to use an S3 Bucket Key for server-side encryption with KMS
     * keys (SSE-KMS).</p>
     */
    inline bool GetBucketKeyEnabled() const{ return m_bucketKeyEnabled; }
    inline void SetBucketKeyEnabled(bool value) { m_bucketKeyEnabled = value; }
    inline CreateSessionResult& WithBucketKeyEnabled(bool value) { SetBucketKeyEnabled(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The established temporary security credentials for the created session.</p>
     */
    inline const SessionCredentials& GetCredentials() const{ return m_credentials; }
    inline void SetCredentials(const SessionCredentials& value) { m_credentials = value; }
    inline void SetCredentials(SessionCredentials&& value) { m_credentials = std::move(value); }
    inline CreateSessionResult& WithCredentials(const SessionCredentials& value) { SetCredentials(value); return *this;}
    inline CreateSessionResult& WithCredentials(SessionCredentials&& value) { SetCredentials(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline CreateSessionResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline CreateSessionResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline CreateSessionResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    ServerSideEncryption m_serverSideEncryption;

    Aws::String m_sSEKMSKeyId;

    Aws::String m_sSEKMSEncryptionContext;

    bool m_bucketKeyEnabled;

    SessionCredentials m_credentials;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
