/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/InventoryFormat.h>
#include <aws/s3/model/InventoryEncryption.h>
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
   * <p>Contains the bucket name, file format, bucket owner (optional), and prefix
   * (optional) where inventory results are published.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/InventoryS3BucketDestination">AWS
   * API Reference</a></p>
   */
  class InventoryS3BucketDestination
  {
  public:
    AWS_S3_API InventoryS3BucketDestination();
    AWS_S3_API InventoryS3BucketDestination(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API InventoryS3BucketDestination& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The account ID that owns the destination S3 bucket. If no account ID is
     * provided, the owner is not validated before exporting data. </p>  <p>
     * Although this value is optional, we strongly recommend that you set it to help
     * prevent problems if the destination bucket ownership changes. </p> 
     */
    inline const Aws::String& GetAccountId() const{ return m_accountId; }
    inline bool AccountIdHasBeenSet() const { return m_accountIdHasBeenSet; }
    inline void SetAccountId(const Aws::String& value) { m_accountIdHasBeenSet = true; m_accountId = value; }
    inline void SetAccountId(Aws::String&& value) { m_accountIdHasBeenSet = true; m_accountId = std::move(value); }
    inline void SetAccountId(const char* value) { m_accountIdHasBeenSet = true; m_accountId.assign(value); }
    inline InventoryS3BucketDestination& WithAccountId(const Aws::String& value) { SetAccountId(value); return *this;}
    inline InventoryS3BucketDestination& WithAccountId(Aws::String&& value) { SetAccountId(std::move(value)); return *this;}
    inline InventoryS3BucketDestination& WithAccountId(const char* value) { SetAccountId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the bucket where inventory results will be
     * published.</p>
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline bool BucketHasBeenSet() const { return m_bucketHasBeenSet; }
    inline void SetBucket(const Aws::String& value) { m_bucketHasBeenSet = true; m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucketHasBeenSet = true; m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucketHasBeenSet = true; m_bucket.assign(value); }
    inline InventoryS3BucketDestination& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline InventoryS3BucketDestination& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline InventoryS3BucketDestination& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the output format of the inventory results.</p>
     */
    inline const InventoryFormat& GetFormat() const{ return m_format; }
    inline bool FormatHasBeenSet() const { return m_formatHasBeenSet; }
    inline void SetFormat(const InventoryFormat& value) { m_formatHasBeenSet = true; m_format = value; }
    inline void SetFormat(InventoryFormat&& value) { m_formatHasBeenSet = true; m_format = std::move(value); }
    inline InventoryS3BucketDestination& WithFormat(const InventoryFormat& value) { SetFormat(value); return *this;}
    inline InventoryS3BucketDestination& WithFormat(InventoryFormat&& value) { SetFormat(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The prefix that is prepended to all inventory results.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline InventoryS3BucketDestination& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline InventoryS3BucketDestination& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline InventoryS3BucketDestination& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Contains the type of server-side encryption used to encrypt the inventory
     * results.</p>
     */
    inline const InventoryEncryption& GetEncryption() const{ return m_encryption; }
    inline bool EncryptionHasBeenSet() const { return m_encryptionHasBeenSet; }
    inline void SetEncryption(const InventoryEncryption& value) { m_encryptionHasBeenSet = true; m_encryption = value; }
    inline void SetEncryption(InventoryEncryption&& value) { m_encryptionHasBeenSet = true; m_encryption = std::move(value); }
    inline InventoryS3BucketDestination& WithEncryption(const InventoryEncryption& value) { SetEncryption(value); return *this;}
    inline InventoryS3BucketDestination& WithEncryption(InventoryEncryption&& value) { SetEncryption(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_accountId;
    bool m_accountIdHasBeenSet = false;

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    InventoryFormat m_format;
    bool m_formatHasBeenSet = false;

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    InventoryEncryption m_encryption;
    bool m_encryptionHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
