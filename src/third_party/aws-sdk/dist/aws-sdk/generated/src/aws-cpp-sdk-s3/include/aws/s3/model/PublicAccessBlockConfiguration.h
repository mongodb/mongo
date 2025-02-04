/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>

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
   * <p>The PublicAccessBlock configuration that you want to apply to this Amazon S3
   * bucket. You can enable the configuration options in any combination. For more
   * information about when Amazon S3 considers a bucket or object public, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/access-control-block-public-access.html#access-control-block-public-access-policy-status">The
   * Meaning of "Public"</a> in the <i>Amazon S3 User Guide</i>. </p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PublicAccessBlockConfiguration">AWS
   * API Reference</a></p>
   */
  class PublicAccessBlockConfiguration
  {
  public:
    AWS_S3_API PublicAccessBlockConfiguration();
    AWS_S3_API PublicAccessBlockConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API PublicAccessBlockConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies whether Amazon S3 should block public access control lists (ACLs)
     * for this bucket and objects in this bucket. Setting this element to
     * <code>TRUE</code> causes the following behavior:</p> <ul> <li> <p>PUT Bucket ACL
     * and PUT Object ACL calls fail if the specified ACL is public.</p> </li> <li>
     * <p>PUT Object calls fail if the request includes a public ACL.</p> </li> <li>
     * <p>PUT Bucket calls fail if the request includes a public ACL.</p> </li> </ul>
     * <p>Enabling this setting doesn't affect existing policies or ACLs.</p>
     */
    inline bool GetBlockPublicAcls() const{ return m_blockPublicAcls; }
    inline bool BlockPublicAclsHasBeenSet() const { return m_blockPublicAclsHasBeenSet; }
    inline void SetBlockPublicAcls(bool value) { m_blockPublicAclsHasBeenSet = true; m_blockPublicAcls = value; }
    inline PublicAccessBlockConfiguration& WithBlockPublicAcls(bool value) { SetBlockPublicAcls(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether Amazon S3 should ignore public ACLs for this bucket and
     * objects in this bucket. Setting this element to <code>TRUE</code> causes Amazon
     * S3 to ignore all public ACLs on this bucket and objects in this bucket.</p>
     * <p>Enabling this setting doesn't affect the persistence of any existing ACLs and
     * doesn't prevent new public ACLs from being set.</p>
     */
    inline bool GetIgnorePublicAcls() const{ return m_ignorePublicAcls; }
    inline bool IgnorePublicAclsHasBeenSet() const { return m_ignorePublicAclsHasBeenSet; }
    inline void SetIgnorePublicAcls(bool value) { m_ignorePublicAclsHasBeenSet = true; m_ignorePublicAcls = value; }
    inline PublicAccessBlockConfiguration& WithIgnorePublicAcls(bool value) { SetIgnorePublicAcls(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether Amazon S3 should block public bucket policies for this
     * bucket. Setting this element to <code>TRUE</code> causes Amazon S3 to reject
     * calls to PUT Bucket policy if the specified bucket policy allows public access.
     * </p> <p>Enabling this setting doesn't affect existing bucket policies.</p>
     */
    inline bool GetBlockPublicPolicy() const{ return m_blockPublicPolicy; }
    inline bool BlockPublicPolicyHasBeenSet() const { return m_blockPublicPolicyHasBeenSet; }
    inline void SetBlockPublicPolicy(bool value) { m_blockPublicPolicyHasBeenSet = true; m_blockPublicPolicy = value; }
    inline PublicAccessBlockConfiguration& WithBlockPublicPolicy(bool value) { SetBlockPublicPolicy(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether Amazon S3 should restrict public bucket policies for this
     * bucket. Setting this element to <code>TRUE</code> restricts access to this
     * bucket to only Amazon Web Services service principals and authorized users
     * within this account if the bucket has a public policy.</p> <p>Enabling this
     * setting doesn't affect previously stored bucket policies, except that public and
     * cross-account access within any public bucket policy, including non-public
     * delegation to specific accounts, is blocked.</p>
     */
    inline bool GetRestrictPublicBuckets() const{ return m_restrictPublicBuckets; }
    inline bool RestrictPublicBucketsHasBeenSet() const { return m_restrictPublicBucketsHasBeenSet; }
    inline void SetRestrictPublicBuckets(bool value) { m_restrictPublicBucketsHasBeenSet = true; m_restrictPublicBuckets = value; }
    inline PublicAccessBlockConfiguration& WithRestrictPublicBuckets(bool value) { SetRestrictPublicBuckets(value); return *this;}
    ///@}
  private:

    bool m_blockPublicAcls;
    bool m_blockPublicAclsHasBeenSet = false;

    bool m_ignorePublicAcls;
    bool m_ignorePublicAclsHasBeenSet = false;

    bool m_blockPublicPolicy;
    bool m_blockPublicPolicyHasBeenSet = false;

    bool m_restrictPublicBuckets;
    bool m_restrictPublicBucketsHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
