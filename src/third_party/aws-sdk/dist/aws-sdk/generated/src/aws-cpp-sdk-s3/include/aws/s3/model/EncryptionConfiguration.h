/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
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
   * <p>Specifies encryption-related information for an Amazon S3 bucket that is a
   * destination for replicated objects.</p>  <p>If you're specifying a
   * customer managed KMS key, we recommend using a fully qualified KMS key ARN. If
   * you use a KMS key alias instead, then KMS resolves the key within the
   * requester’s account. This behavior can result in data that's encrypted with a
   * KMS key that belongs to the requester, and not the bucket owner.</p>
   * <p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/EncryptionConfiguration">AWS
   * API Reference</a></p>
   */
  class EncryptionConfiguration
  {
  public:
    AWS_S3_API EncryptionConfiguration();
    AWS_S3_API EncryptionConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API EncryptionConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies the ID (Key ARN or Alias ARN) of the customer managed Amazon Web
     * Services KMS key stored in Amazon Web Services Key Management Service (KMS) for
     * the destination bucket. Amazon S3 uses this key to encrypt replica objects.
     * Amazon S3 only supports symmetric encryption KMS keys. For more information, see
     * <a
     * href="https://docs.aws.amazon.com/kms/latest/developerguide/symmetric-asymmetric.html">Asymmetric
     * keys in Amazon Web Services KMS</a> in the <i>Amazon Web Services Key Management
     * Service Developer Guide</i>.</p>
     */
    inline const Aws::String& GetReplicaKmsKeyID() const{ return m_replicaKmsKeyID; }
    inline bool ReplicaKmsKeyIDHasBeenSet() const { return m_replicaKmsKeyIDHasBeenSet; }
    inline void SetReplicaKmsKeyID(const Aws::String& value) { m_replicaKmsKeyIDHasBeenSet = true; m_replicaKmsKeyID = value; }
    inline void SetReplicaKmsKeyID(Aws::String&& value) { m_replicaKmsKeyIDHasBeenSet = true; m_replicaKmsKeyID = std::move(value); }
    inline void SetReplicaKmsKeyID(const char* value) { m_replicaKmsKeyIDHasBeenSet = true; m_replicaKmsKeyID.assign(value); }
    inline EncryptionConfiguration& WithReplicaKmsKeyID(const Aws::String& value) { SetReplicaKmsKeyID(value); return *this;}
    inline EncryptionConfiguration& WithReplicaKmsKeyID(Aws::String&& value) { SetReplicaKmsKeyID(std::move(value)); return *this;}
    inline EncryptionConfiguration& WithReplicaKmsKeyID(const char* value) { SetReplicaKmsKeyID(value); return *this;}
    ///@}
  private:

    Aws::String m_replicaKmsKeyID;
    bool m_replicaKmsKeyIDHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
