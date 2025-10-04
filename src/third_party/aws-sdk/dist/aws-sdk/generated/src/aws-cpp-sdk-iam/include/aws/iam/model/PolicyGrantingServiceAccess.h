/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/PolicyType.h>
#include <aws/iam/model/PolicyOwnerEntityType.h>
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
namespace IAM
{
namespace Model
{

  /**
   * <p>Contains details about the permissions policies that are attached to the
   * specified identity (user, group, or role).</p> <p>This data type is an element
   * of the <a>ListPoliciesGrantingServiceAccessEntry</a> object.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/PolicyGrantingServiceAccess">AWS
   * API Reference</a></p>
   */
  class PolicyGrantingServiceAccess
  {
  public:
    AWS_IAM_API PolicyGrantingServiceAccess();
    AWS_IAM_API PolicyGrantingServiceAccess(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API PolicyGrantingServiceAccess& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The policy name.</p>
     */
    inline const Aws::String& GetPolicyName() const{ return m_policyName; }
    inline bool PolicyNameHasBeenSet() const { return m_policyNameHasBeenSet; }
    inline void SetPolicyName(const Aws::String& value) { m_policyNameHasBeenSet = true; m_policyName = value; }
    inline void SetPolicyName(Aws::String&& value) { m_policyNameHasBeenSet = true; m_policyName = std::move(value); }
    inline void SetPolicyName(const char* value) { m_policyNameHasBeenSet = true; m_policyName.assign(value); }
    inline PolicyGrantingServiceAccess& WithPolicyName(const Aws::String& value) { SetPolicyName(value); return *this;}
    inline PolicyGrantingServiceAccess& WithPolicyName(Aws::String&& value) { SetPolicyName(std::move(value)); return *this;}
    inline PolicyGrantingServiceAccess& WithPolicyName(const char* value) { SetPolicyName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The policy type. For more information about these policy types, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_managed-vs-inline.html">Managed
     * policies and inline policies</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const PolicyType& GetPolicyType() const{ return m_policyType; }
    inline bool PolicyTypeHasBeenSet() const { return m_policyTypeHasBeenSet; }
    inline void SetPolicyType(const PolicyType& value) { m_policyTypeHasBeenSet = true; m_policyType = value; }
    inline void SetPolicyType(PolicyType&& value) { m_policyTypeHasBeenSet = true; m_policyType = std::move(value); }
    inline PolicyGrantingServiceAccess& WithPolicyType(const PolicyType& value) { SetPolicyType(value); return *this;}
    inline PolicyGrantingServiceAccess& WithPolicyType(PolicyType&& value) { SetPolicyType(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetPolicyArn() const{ return m_policyArn; }
    inline bool PolicyArnHasBeenSet() const { return m_policyArnHasBeenSet; }
    inline void SetPolicyArn(const Aws::String& value) { m_policyArnHasBeenSet = true; m_policyArn = value; }
    inline void SetPolicyArn(Aws::String&& value) { m_policyArnHasBeenSet = true; m_policyArn = std::move(value); }
    inline void SetPolicyArn(const char* value) { m_policyArnHasBeenSet = true; m_policyArn.assign(value); }
    inline PolicyGrantingServiceAccess& WithPolicyArn(const Aws::String& value) { SetPolicyArn(value); return *this;}
    inline PolicyGrantingServiceAccess& WithPolicyArn(Aws::String&& value) { SetPolicyArn(std::move(value)); return *this;}
    inline PolicyGrantingServiceAccess& WithPolicyArn(const char* value) { SetPolicyArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The type of entity (user or role) that used the policy to access the service
     * to which the inline policy is attached.</p> <p>This field is null for managed
     * policies. For more information about these policy types, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_managed-vs-inline.html">Managed
     * policies and inline policies</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const PolicyOwnerEntityType& GetEntityType() const{ return m_entityType; }
    inline bool EntityTypeHasBeenSet() const { return m_entityTypeHasBeenSet; }
    inline void SetEntityType(const PolicyOwnerEntityType& value) { m_entityTypeHasBeenSet = true; m_entityType = value; }
    inline void SetEntityType(PolicyOwnerEntityType&& value) { m_entityTypeHasBeenSet = true; m_entityType = std::move(value); }
    inline PolicyGrantingServiceAccess& WithEntityType(const PolicyOwnerEntityType& value) { SetEntityType(value); return *this;}
    inline PolicyGrantingServiceAccess& WithEntityType(PolicyOwnerEntityType&& value) { SetEntityType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the entity (user or role) to which the inline policy is
     * attached.</p> <p>This field is null for managed policies. For more information
     * about these policy types, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_managed-vs-inline.html">Managed
     * policies and inline policies</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetEntityName() const{ return m_entityName; }
    inline bool EntityNameHasBeenSet() const { return m_entityNameHasBeenSet; }
    inline void SetEntityName(const Aws::String& value) { m_entityNameHasBeenSet = true; m_entityName = value; }
    inline void SetEntityName(Aws::String&& value) { m_entityNameHasBeenSet = true; m_entityName = std::move(value); }
    inline void SetEntityName(const char* value) { m_entityNameHasBeenSet = true; m_entityName.assign(value); }
    inline PolicyGrantingServiceAccess& WithEntityName(const Aws::String& value) { SetEntityName(value); return *this;}
    inline PolicyGrantingServiceAccess& WithEntityName(Aws::String&& value) { SetEntityName(std::move(value)); return *this;}
    inline PolicyGrantingServiceAccess& WithEntityName(const char* value) { SetEntityName(value); return *this;}
    ///@}
  private:

    Aws::String m_policyName;
    bool m_policyNameHasBeenSet = false;

    PolicyType m_policyType;
    bool m_policyTypeHasBeenSet = false;

    Aws::String m_policyArn;
    bool m_policyArnHasBeenSet = false;

    PolicyOwnerEntityType m_entityType;
    bool m_entityTypeHasBeenSet = false;

    Aws::String m_entityName;
    bool m_entityNameHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
