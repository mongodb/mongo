/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ServerSideEncryptionByDefault.h>
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
   * <p>Specifies the default server-side encryption configuration.</p>  <ul>
   * <li> <p> <b>General purpose buckets</b> - If you're specifying a customer
   * managed KMS key, we recommend using a fully qualified KMS key ARN. If you use a
   * KMS key alias instead, then KMS resolves the key within the requester’s account.
   * This behavior can result in data that's encrypted with a KMS key that belongs to
   * the requester, and not the bucket owner.</p> </li> <li> <p> <b>Directory
   * buckets</b> - When you specify an <a
   * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#customer-cmk">KMS
   * customer managed key</a> for encryption in your directory bucket, only use the
   * key ID or key ARN. The key alias format of the KMS key isn't supported.</p>
   * </li> </ul> <p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ServerSideEncryptionRule">AWS
   * API Reference</a></p>
   */
  class ServerSideEncryptionRule
  {
  public:
    AWS_S3_API ServerSideEncryptionRule();
    AWS_S3_API ServerSideEncryptionRule(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ServerSideEncryptionRule& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies the default server-side encryption to apply to new objects in the
     * bucket. If a PUT Object request doesn't specify any server-side encryption, this
     * default encryption will be applied.</p>
     */
    inline const ServerSideEncryptionByDefault& GetApplyServerSideEncryptionByDefault() const{ return m_applyServerSideEncryptionByDefault; }
    inline bool ApplyServerSideEncryptionByDefaultHasBeenSet() const { return m_applyServerSideEncryptionByDefaultHasBeenSet; }
    inline void SetApplyServerSideEncryptionByDefault(const ServerSideEncryptionByDefault& value) { m_applyServerSideEncryptionByDefaultHasBeenSet = true; m_applyServerSideEncryptionByDefault = value; }
    inline void SetApplyServerSideEncryptionByDefault(ServerSideEncryptionByDefault&& value) { m_applyServerSideEncryptionByDefaultHasBeenSet = true; m_applyServerSideEncryptionByDefault = std::move(value); }
    inline ServerSideEncryptionRule& WithApplyServerSideEncryptionByDefault(const ServerSideEncryptionByDefault& value) { SetApplyServerSideEncryptionByDefault(value); return *this;}
    inline ServerSideEncryptionRule& WithApplyServerSideEncryptionByDefault(ServerSideEncryptionByDefault&& value) { SetApplyServerSideEncryptionByDefault(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether Amazon S3 should use an S3 Bucket Key with server-side
     * encryption using KMS (SSE-KMS) for new objects in the bucket. Existing objects
     * are not affected. Setting the <code>BucketKeyEnabled</code> element to
     * <code>true</code> causes Amazon S3 to use an S3 Bucket Key. </p>  <ul>
     * <li> <p> <b>General purpose buckets</b> - By default, S3 Bucket Key is not
     * enabled. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/bucket-key.html">Amazon S3
     * Bucket Keys</a> in the <i>Amazon S3 User Guide</i>.</p> </li> <li> <p>
     * <b>Directory buckets</b> - S3 Bucket Keys are always enabled for
     * <code>GET</code> and <code>PUT</code> operations in a directory bucket and can’t
     * be disabled. S3 Bucket Keys aren't supported, when you copy SSE-KMS encrypted
     * objects from general purpose buckets to directory buckets, from directory
     * buckets to general purpose buckets, or between directory buckets, through <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>,
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>,
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/directory-buckets-objects-Batch-Ops">the
     * Copy operation in Batch Operations</a>, or <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/create-import-job">the
     * import jobs</a>. In this case, Amazon S3 makes a call to KMS every time a copy
     * request is made for a KMS-encrypted object.</p> </li> </ul> 
     */
    inline bool GetBucketKeyEnabled() const{ return m_bucketKeyEnabled; }
    inline bool BucketKeyEnabledHasBeenSet() const { return m_bucketKeyEnabledHasBeenSet; }
    inline void SetBucketKeyEnabled(bool value) { m_bucketKeyEnabledHasBeenSet = true; m_bucketKeyEnabled = value; }
    inline ServerSideEncryptionRule& WithBucketKeyEnabled(bool value) { SetBucketKeyEnabled(value); return *this;}
    ///@}
  private:

    ServerSideEncryptionByDefault m_applyServerSideEncryptionByDefault;
    bool m_applyServerSideEncryptionByDefaultHasBeenSet = false;

    bool m_bucketKeyEnabled;
    bool m_bucketKeyEnabledHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
