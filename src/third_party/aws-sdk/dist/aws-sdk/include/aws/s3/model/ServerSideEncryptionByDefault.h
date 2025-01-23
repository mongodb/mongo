/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ServerSideEncryption.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{

  /**
   * <p>Describes the default server-side encryption to apply to new objects in the
   * bucket. If a PUT Object request doesn't specify any server-side encryption, this
   * default encryption will be applied. For more information, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTBucketPUTencryption.html">PutBucketEncryption</a>.</p>
   *  <ul> <li> <p> <b>General purpose buckets</b> - If you don't specify a
   * customer managed key at configuration, Amazon S3 automatically creates an Amazon
   * Web Services KMS key (<code>aws/s3</code>) in your Amazon Web Services account
   * the first time that you add an object encrypted with SSE-KMS to a bucket. By
   * default, Amazon S3 uses this KMS key for SSE-KMS. </p> </li> <li> <p>
   * <b>Directory buckets</b> - Your SSE-KMS configuration can only support 1 <a
   * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#customer-cmk">customer
   * managed key</a> per directory bucket for the lifetime of the bucket. The <a
   * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#aws-managed-cmk">Amazon
   * Web Services managed key</a> (<code>aws/s3</code>) isn't supported. </p> </li>
   * <li> <p> <b>Directory buckets</b> - For directory buckets, there are only two
   * supported options for server-side encryption: SSE-S3 and SSE-KMS.</p> </li>
   * </ul> <p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ServerSideEncryptionByDefault">AWS
   * API Reference</a></p>
   */
  class ServerSideEncryptionByDefault
  {
  public:
    AWS_S3_API ServerSideEncryptionByDefault();
    AWS_S3_API ServerSideEncryptionByDefault(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ServerSideEncryptionByDefault& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Server-side encryption algorithm to use for the default encryption.</p>
     *  <p>For directory buckets, there are only two supported values for
     * server-side encryption: <code>AES256</code> and <code>aws:kms</code>.</p>
     * 
     */
    inline const ServerSideEncryption& GetSSEAlgorithm() const{ return m_sSEAlgorithm; }
    inline bool SSEAlgorithmHasBeenSet() const { return m_sSEAlgorithmHasBeenSet; }
    inline void SetSSEAlgorithm(const ServerSideEncryption& value) { m_sSEAlgorithmHasBeenSet = true; m_sSEAlgorithm = value; }
    inline void SetSSEAlgorithm(ServerSideEncryption&& value) { m_sSEAlgorithmHasBeenSet = true; m_sSEAlgorithm = std::move(value); }
    inline ServerSideEncryptionByDefault& WithSSEAlgorithm(const ServerSideEncryption& value) { SetSSEAlgorithm(value); return *this;}
    inline ServerSideEncryptionByDefault& WithSSEAlgorithm(ServerSideEncryption&& value) { SetSSEAlgorithm(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Amazon Web Services Key Management Service (KMS) customer managed key ID to
     * use for the default encryption. </p>  <ul> <li> <p> <b>General purpose
     * buckets</b> - This parameter is allowed if and only if <code>SSEAlgorithm</code>
     * is set to <code>aws:kms</code> or <code>aws:kms:dsse</code>.</p> </li> <li> <p>
     * <b>Directory buckets</b> - This parameter is allowed if and only if
     * <code>SSEAlgorithm</code> is set to <code>aws:kms</code>.</p> </li> </ul>
     *  <p>You can specify the key ID, key alias, or the Amazon Resource Name
     * (ARN) of the KMS key.</p> <ul> <li> <p>Key ID:
     * <code>1234abcd-12ab-34cd-56ef-1234567890ab</code> </p> </li> <li> <p>Key ARN:
     * <code>arn:aws:kms:us-east-2:111122223333:key/1234abcd-12ab-34cd-56ef-1234567890ab</code>
     * </p> </li> <li> <p>Key Alias: <code>alias/alias-name</code> </p> </li> </ul>
     * <p>If you are using encryption with cross-account or Amazon Web Services service
     * operations, you must use a fully qualified KMS key ARN. For more information,
     * see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/bucket-encryption.html#bucket-encryption-update-bucket-policy">Using
     * encryption for cross-account operations</a>.</p>  <ul> <li> <p> <b>General
     * purpose buckets</b> - If you're specifying a customer managed KMS key, we
     * recommend using a fully qualified KMS key ARN. If you use a KMS key alias
     * instead, then KMS resolves the key within the requester’s account. This behavior
     * can result in data that's encrypted with a KMS key that belongs to the
     * requester, and not the bucket owner. Also, if you use a key ID, you can run into
     * a LogDestination undeliverable error when creating a VPC flow log. </p> </li>
     * <li> <p> <b>Directory buckets</b> - When you specify an <a
     * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#customer-cmk">KMS
     * customer managed key</a> for encryption in your directory bucket, only use the
     * key ID or key ARN. The key alias format of the KMS key isn't supported.</p>
     * </li> </ul>   <p>Amazon S3 only supports symmetric encryption
     * KMS keys. For more information, see <a
     * href="https://docs.aws.amazon.com/kms/latest/developerguide/symmetric-asymmetric.html">Asymmetric
     * keys in Amazon Web Services KMS</a> in the <i>Amazon Web Services Key Management
     * Service Developer Guide</i>.</p> 
     */
    inline const Aws::String& GetKMSMasterKeyID() const{ return m_kMSMasterKeyID; }
    inline bool KMSMasterKeyIDHasBeenSet() const { return m_kMSMasterKeyIDHasBeenSet; }
    inline void SetKMSMasterKeyID(const Aws::String& value) { m_kMSMasterKeyIDHasBeenSet = true; m_kMSMasterKeyID = value; }
    inline void SetKMSMasterKeyID(Aws::String&& value) { m_kMSMasterKeyIDHasBeenSet = true; m_kMSMasterKeyID = std::move(value); }
    inline void SetKMSMasterKeyID(const char* value) { m_kMSMasterKeyIDHasBeenSet = true; m_kMSMasterKeyID.assign(value); }
    inline ServerSideEncryptionByDefault& WithKMSMasterKeyID(const Aws::String& value) { SetKMSMasterKeyID(value); return *this;}
    inline ServerSideEncryptionByDefault& WithKMSMasterKeyID(Aws::String&& value) { SetKMSMasterKeyID(std::move(value)); return *this;}
    inline ServerSideEncryptionByDefault& WithKMSMasterKeyID(const char* value) { SetKMSMasterKeyID(value); return *this;}
    ///@}
  private:

    ServerSideEncryption m_sSEAlgorithm;
    bool m_sSEAlgorithmHasBeenSet = false;

    Aws::String m_kMSMasterKeyID;
    bool m_kMSMasterKeyIDHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
