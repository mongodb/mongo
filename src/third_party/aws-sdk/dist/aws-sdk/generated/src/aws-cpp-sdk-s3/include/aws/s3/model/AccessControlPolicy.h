/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/Owner.h>
#include <aws/s3/model/Grant.h>
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
   * <p>Contains the elements that set the ACL permissions for an object per
   * grantee.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/AccessControlPolicy">AWS
   * API Reference</a></p>
   */
  class AccessControlPolicy
  {
  public:
    AWS_S3_API AccessControlPolicy();
    AWS_S3_API AccessControlPolicy(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API AccessControlPolicy& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>A list of grants.</p>
     */
    inline const Aws::Vector<Grant>& GetGrants() const{ return m_grants; }
    inline bool GrantsHasBeenSet() const { return m_grantsHasBeenSet; }
    inline void SetGrants(const Aws::Vector<Grant>& value) { m_grantsHasBeenSet = true; m_grants = value; }
    inline void SetGrants(Aws::Vector<Grant>&& value) { m_grantsHasBeenSet = true; m_grants = std::move(value); }
    inline AccessControlPolicy& WithGrants(const Aws::Vector<Grant>& value) { SetGrants(value); return *this;}
    inline AccessControlPolicy& WithGrants(Aws::Vector<Grant>&& value) { SetGrants(std::move(value)); return *this;}
    inline AccessControlPolicy& AddGrants(const Grant& value) { m_grantsHasBeenSet = true; m_grants.push_back(value); return *this; }
    inline AccessControlPolicy& AddGrants(Grant&& value) { m_grantsHasBeenSet = true; m_grants.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Container for the bucket owner's display name and ID.</p>
     */
    inline const Owner& GetOwner() const{ return m_owner; }
    inline bool OwnerHasBeenSet() const { return m_ownerHasBeenSet; }
    inline void SetOwner(const Owner& value) { m_ownerHasBeenSet = true; m_owner = value; }
    inline void SetOwner(Owner&& value) { m_ownerHasBeenSet = true; m_owner = std::move(value); }
    inline AccessControlPolicy& WithOwner(const Owner& value) { SetOwner(value); return *this;}
    inline AccessControlPolicy& WithOwner(Owner&& value) { SetOwner(std::move(value)); return *this;}
    ///@}
  private:

    Aws::Vector<Grant> m_grants;
    bool m_grantsHasBeenSet = false;

    Owner m_owner;
    bool m_ownerHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
