/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/ContextEntry.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class SimulateCustomPolicyRequest : public IAMRequest
  {
  public:
    AWS_IAM_API SimulateCustomPolicyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "SimulateCustomPolicy"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>A list of policy documents to include in the simulation. Each document is
     * specified as a string containing the complete, valid JSON text of an IAM policy.
     * Do not include any resource-based policies in this parameter. Any resource-based
     * policy must be submitted with the <code>ResourcePolicy</code> parameter. The
     * policies cannot be "scope-down" policies, such as you could include in a call to
     * <a
     * href="https://docs.aws.amazon.com/IAM/latest/APIReference/API_GetFederationToken.html">GetFederationToken</a>
     * or one of the <a
     * href="https://docs.aws.amazon.com/IAM/latest/APIReference/API_AssumeRole.html">AssumeRole</a>
     * API operations. In other words, do not use policies designed to restrict what a
     * user can do while using the temporary credentials.</p> <p>The maximum length of
     * the policy document that you can pass in this operation, including whitespace,
     * is listed below. To view the maximum character counts of a managed policy with
     * no whitespaces, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_iam-quotas.html#reference_iam-quotas-entity-length">IAM
     * and STS character quotas</a>.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>
     */
    inline const Aws::Vector<Aws::String>& GetPolicyInputList() const{ return m_policyInputList; }
    inline bool PolicyInputListHasBeenSet() const { return m_policyInputListHasBeenSet; }
    inline void SetPolicyInputList(const Aws::Vector<Aws::String>& value) { m_policyInputListHasBeenSet = true; m_policyInputList = value; }
    inline void SetPolicyInputList(Aws::Vector<Aws::String>&& value) { m_policyInputListHasBeenSet = true; m_policyInputList = std::move(value); }
    inline SimulateCustomPolicyRequest& WithPolicyInputList(const Aws::Vector<Aws::String>& value) { SetPolicyInputList(value); return *this;}
    inline SimulateCustomPolicyRequest& WithPolicyInputList(Aws::Vector<Aws::String>&& value) { SetPolicyInputList(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& AddPolicyInputList(const Aws::String& value) { m_policyInputListHasBeenSet = true; m_policyInputList.push_back(value); return *this; }
    inline SimulateCustomPolicyRequest& AddPolicyInputList(Aws::String&& value) { m_policyInputListHasBeenSet = true; m_policyInputList.push_back(std::move(value)); return *this; }
    inline SimulateCustomPolicyRequest& AddPolicyInputList(const char* value) { m_policyInputListHasBeenSet = true; m_policyInputList.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The IAM permissions boundary policy to simulate. The permissions boundary
     * sets the maximum permissions that an IAM entity can have. You can input only one
     * permissions boundary when you pass a policy to this operation. For more
     * information about permissions boundaries, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_boundaries.html">Permissions
     * boundaries for IAM entities</a> in the <i>IAM User Guide</i>. The policy input
     * is specified as a string that contains the complete, valid JSON text of a
     * permissions boundary policy.</p> <p>The maximum length of the policy document
     * that you can pass in this operation, including whitespace, is listed below. To
     * view the maximum character counts of a managed policy with no whitespaces, see
     * <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_iam-quotas.html#reference_iam-quotas-entity-length">IAM
     * and STS character quotas</a>.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>
     */
    inline const Aws::Vector<Aws::String>& GetPermissionsBoundaryPolicyInputList() const{ return m_permissionsBoundaryPolicyInputList; }
    inline bool PermissionsBoundaryPolicyInputListHasBeenSet() const { return m_permissionsBoundaryPolicyInputListHasBeenSet; }
    inline void SetPermissionsBoundaryPolicyInputList(const Aws::Vector<Aws::String>& value) { m_permissionsBoundaryPolicyInputListHasBeenSet = true; m_permissionsBoundaryPolicyInputList = value; }
    inline void SetPermissionsBoundaryPolicyInputList(Aws::Vector<Aws::String>&& value) { m_permissionsBoundaryPolicyInputListHasBeenSet = true; m_permissionsBoundaryPolicyInputList = std::move(value); }
    inline SimulateCustomPolicyRequest& WithPermissionsBoundaryPolicyInputList(const Aws::Vector<Aws::String>& value) { SetPermissionsBoundaryPolicyInputList(value); return *this;}
    inline SimulateCustomPolicyRequest& WithPermissionsBoundaryPolicyInputList(Aws::Vector<Aws::String>&& value) { SetPermissionsBoundaryPolicyInputList(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& AddPermissionsBoundaryPolicyInputList(const Aws::String& value) { m_permissionsBoundaryPolicyInputListHasBeenSet = true; m_permissionsBoundaryPolicyInputList.push_back(value); return *this; }
    inline SimulateCustomPolicyRequest& AddPermissionsBoundaryPolicyInputList(Aws::String&& value) { m_permissionsBoundaryPolicyInputListHasBeenSet = true; m_permissionsBoundaryPolicyInputList.push_back(std::move(value)); return *this; }
    inline SimulateCustomPolicyRequest& AddPermissionsBoundaryPolicyInputList(const char* value) { m_permissionsBoundaryPolicyInputListHasBeenSet = true; m_permissionsBoundaryPolicyInputList.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of names of API operations to evaluate in the simulation. Each
     * operation is evaluated against each resource. Each operation must include the
     * service identifier, such as <code>iam:CreateUser</code>. This operation does not
     * support using wildcards (*) in an action name.</p>
     */
    inline const Aws::Vector<Aws::String>& GetActionNames() const{ return m_actionNames; }
    inline bool ActionNamesHasBeenSet() const { return m_actionNamesHasBeenSet; }
    inline void SetActionNames(const Aws::Vector<Aws::String>& value) { m_actionNamesHasBeenSet = true; m_actionNames = value; }
    inline void SetActionNames(Aws::Vector<Aws::String>&& value) { m_actionNamesHasBeenSet = true; m_actionNames = std::move(value); }
    inline SimulateCustomPolicyRequest& WithActionNames(const Aws::Vector<Aws::String>& value) { SetActionNames(value); return *this;}
    inline SimulateCustomPolicyRequest& WithActionNames(Aws::Vector<Aws::String>&& value) { SetActionNames(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& AddActionNames(const Aws::String& value) { m_actionNamesHasBeenSet = true; m_actionNames.push_back(value); return *this; }
    inline SimulateCustomPolicyRequest& AddActionNames(Aws::String&& value) { m_actionNamesHasBeenSet = true; m_actionNames.push_back(std::move(value)); return *this; }
    inline SimulateCustomPolicyRequest& AddActionNames(const char* value) { m_actionNamesHasBeenSet = true; m_actionNames.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of ARNs of Amazon Web Services resources to include in the simulation.
     * If this parameter is not provided, then the value defaults to <code>*</code>
     * (all resources). Each API in the <code>ActionNames</code> parameter is evaluated
     * for each resource in this list. The simulation determines the access result
     * (allowed or denied) of each combination and reports it in the response. You can
     * simulate resources that don't exist in your account.</p> <p>The simulation does
     * not automatically retrieve policies for the specified resources. If you want to
     * include a resource policy in the simulation, then you must include the policy as
     * a string in the <code>ResourcePolicy</code> parameter.</p> <p>If you include a
     * <code>ResourcePolicy</code>, then it must be applicable to all of the resources
     * included in the simulation or you receive an invalid input error.</p> <p>For
     * more information about ARNs, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">Amazon
     * Resource Names (ARNs)</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>  <p>Simulation of resource-based policies isn't
     * supported for IAM roles.</p> 
     */
    inline const Aws::Vector<Aws::String>& GetResourceArns() const{ return m_resourceArns; }
    inline bool ResourceArnsHasBeenSet() const { return m_resourceArnsHasBeenSet; }
    inline void SetResourceArns(const Aws::Vector<Aws::String>& value) { m_resourceArnsHasBeenSet = true; m_resourceArns = value; }
    inline void SetResourceArns(Aws::Vector<Aws::String>&& value) { m_resourceArnsHasBeenSet = true; m_resourceArns = std::move(value); }
    inline SimulateCustomPolicyRequest& WithResourceArns(const Aws::Vector<Aws::String>& value) { SetResourceArns(value); return *this;}
    inline SimulateCustomPolicyRequest& WithResourceArns(Aws::Vector<Aws::String>&& value) { SetResourceArns(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& AddResourceArns(const Aws::String& value) { m_resourceArnsHasBeenSet = true; m_resourceArns.push_back(value); return *this; }
    inline SimulateCustomPolicyRequest& AddResourceArns(Aws::String&& value) { m_resourceArnsHasBeenSet = true; m_resourceArns.push_back(std::move(value)); return *this; }
    inline SimulateCustomPolicyRequest& AddResourceArns(const char* value) { m_resourceArnsHasBeenSet = true; m_resourceArns.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A resource-based policy to include in the simulation provided as a string.
     * Each resource in the simulation is treated as if it had this policy attached.
     * You can include only one resource-based policy in a simulation.</p> <p>The
     * maximum length of the policy document that you can pass in this operation,
     * including whitespace, is listed below. To view the maximum character counts of a
     * managed policy with no whitespaces, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_iam-quotas.html#reference_iam-quotas-entity-length">IAM
     * and STS character quotas</a>.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>  <p>Simulation of
     * resource-based policies isn't supported for IAM roles.</p> 
     */
    inline const Aws::String& GetResourcePolicy() const{ return m_resourcePolicy; }
    inline bool ResourcePolicyHasBeenSet() const { return m_resourcePolicyHasBeenSet; }
    inline void SetResourcePolicy(const Aws::String& value) { m_resourcePolicyHasBeenSet = true; m_resourcePolicy = value; }
    inline void SetResourcePolicy(Aws::String&& value) { m_resourcePolicyHasBeenSet = true; m_resourcePolicy = std::move(value); }
    inline void SetResourcePolicy(const char* value) { m_resourcePolicyHasBeenSet = true; m_resourcePolicy.assign(value); }
    inline SimulateCustomPolicyRequest& WithResourcePolicy(const Aws::String& value) { SetResourcePolicy(value); return *this;}
    inline SimulateCustomPolicyRequest& WithResourcePolicy(Aws::String&& value) { SetResourcePolicy(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& WithResourcePolicy(const char* value) { SetResourcePolicy(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An ARN representing the Amazon Web Services account ID that specifies the
     * owner of any simulated resource that does not identify its owner in the resource
     * ARN. Examples of resource ARNs include an S3 bucket or object. If
     * <code>ResourceOwner</code> is specified, it is also used as the account owner of
     * any <code>ResourcePolicy</code> included in the simulation. If the
     * <code>ResourceOwner</code> parameter is not specified, then the owner of the
     * resources and the resource policy defaults to the account of the identity
     * provided in <code>CallerArn</code>. This parameter is required only if you
     * specify a resource-based policy and account that owns the resource is different
     * from the account that owns the simulated calling user
     * <code>CallerArn</code>.</p> <p>The ARN for an account uses the following syntax:
     * <code>arn:aws:iam::<i>AWS-account-ID</i>:root</code>. For example, to represent
     * the account with the 112233445566 ID, use the following ARN:
     * <code>arn:aws:iam::112233445566-ID:root</code>. </p>
     */
    inline const Aws::String& GetResourceOwner() const{ return m_resourceOwner; }
    inline bool ResourceOwnerHasBeenSet() const { return m_resourceOwnerHasBeenSet; }
    inline void SetResourceOwner(const Aws::String& value) { m_resourceOwnerHasBeenSet = true; m_resourceOwner = value; }
    inline void SetResourceOwner(Aws::String&& value) { m_resourceOwnerHasBeenSet = true; m_resourceOwner = std::move(value); }
    inline void SetResourceOwner(const char* value) { m_resourceOwnerHasBeenSet = true; m_resourceOwner.assign(value); }
    inline SimulateCustomPolicyRequest& WithResourceOwner(const Aws::String& value) { SetResourceOwner(value); return *this;}
    inline SimulateCustomPolicyRequest& WithResourceOwner(Aws::String&& value) { SetResourceOwner(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& WithResourceOwner(const char* value) { SetResourceOwner(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the IAM user that you want to use as the simulated caller of the
     * API operations. <code>CallerArn</code> is required if you include a
     * <code>ResourcePolicy</code> so that the policy's <code>Principal</code> element
     * has a value to use in evaluating the policy.</p> <p>You can specify only the ARN
     * of an IAM user. You cannot specify the ARN of an assumed role, federated user,
     * or a service principal.</p>
     */
    inline const Aws::String& GetCallerArn() const{ return m_callerArn; }
    inline bool CallerArnHasBeenSet() const { return m_callerArnHasBeenSet; }
    inline void SetCallerArn(const Aws::String& value) { m_callerArnHasBeenSet = true; m_callerArn = value; }
    inline void SetCallerArn(Aws::String&& value) { m_callerArnHasBeenSet = true; m_callerArn = std::move(value); }
    inline void SetCallerArn(const char* value) { m_callerArnHasBeenSet = true; m_callerArn.assign(value); }
    inline SimulateCustomPolicyRequest& WithCallerArn(const Aws::String& value) { SetCallerArn(value); return *this;}
    inline SimulateCustomPolicyRequest& WithCallerArn(Aws::String&& value) { SetCallerArn(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& WithCallerArn(const char* value) { SetCallerArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of context keys and corresponding values for the simulation to use.
     * Whenever a context key is evaluated in one of the simulated IAM permissions
     * policies, the corresponding value is supplied.</p>
     */
    inline const Aws::Vector<ContextEntry>& GetContextEntries() const{ return m_contextEntries; }
    inline bool ContextEntriesHasBeenSet() const { return m_contextEntriesHasBeenSet; }
    inline void SetContextEntries(const Aws::Vector<ContextEntry>& value) { m_contextEntriesHasBeenSet = true; m_contextEntries = value; }
    inline void SetContextEntries(Aws::Vector<ContextEntry>&& value) { m_contextEntriesHasBeenSet = true; m_contextEntries = std::move(value); }
    inline SimulateCustomPolicyRequest& WithContextEntries(const Aws::Vector<ContextEntry>& value) { SetContextEntries(value); return *this;}
    inline SimulateCustomPolicyRequest& WithContextEntries(Aws::Vector<ContextEntry>&& value) { SetContextEntries(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& AddContextEntries(const ContextEntry& value) { m_contextEntriesHasBeenSet = true; m_contextEntries.push_back(value); return *this; }
    inline SimulateCustomPolicyRequest& AddContextEntries(ContextEntry&& value) { m_contextEntriesHasBeenSet = true; m_contextEntries.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Specifies the type of simulation to run. Different API operations that
     * support resource-based policies require different combinations of resources. By
     * specifying the type of simulation to run, you enable the policy simulator to
     * enforce the presence of the required resources to ensure reliable simulation
     * results. If your simulation does not match one of the following scenarios, then
     * you can omit this parameter. The following list shows each of the supported
     * scenario values and the resources that you must define to run the
     * simulation.</p> <p>Each of the Amazon EC2 scenarios requires that you specify
     * instance, image, and security group resources. If your scenario includes an EBS
     * volume, then you must specify that volume as a resource. If the Amazon EC2
     * scenario includes VPC, then you must supply the network interface resource. If
     * it includes an IP subnet, then you must specify the subnet resource. For more
     * information on the Amazon EC2 scenario options, see <a
     * href="https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ec2-supported-platforms.html">Supported
     * platforms</a> in the <i>Amazon EC2 User Guide</i>.</p> <ul> <li> <p>
     * <b>EC2-VPC-InstanceStore</b> </p> <p>instance, image, security group, network
     * interface</p> </li> <li> <p> <b>EC2-VPC-InstanceStore-Subnet</b> </p>
     * <p>instance, image, security group, network interface, subnet</p> </li> <li> <p>
     * <b>EC2-VPC-EBS</b> </p> <p>instance, image, security group, network interface,
     * volume</p> </li> <li> <p> <b>EC2-VPC-EBS-Subnet</b> </p> <p>instance, image,
     * security group, network interface, subnet, volume</p> </li> </ul>
     */
    inline const Aws::String& GetResourceHandlingOption() const{ return m_resourceHandlingOption; }
    inline bool ResourceHandlingOptionHasBeenSet() const { return m_resourceHandlingOptionHasBeenSet; }
    inline void SetResourceHandlingOption(const Aws::String& value) { m_resourceHandlingOptionHasBeenSet = true; m_resourceHandlingOption = value; }
    inline void SetResourceHandlingOption(Aws::String&& value) { m_resourceHandlingOptionHasBeenSet = true; m_resourceHandlingOption = std::move(value); }
    inline void SetResourceHandlingOption(const char* value) { m_resourceHandlingOptionHasBeenSet = true; m_resourceHandlingOption.assign(value); }
    inline SimulateCustomPolicyRequest& WithResourceHandlingOption(const Aws::String& value) { SetResourceHandlingOption(value); return *this;}
    inline SimulateCustomPolicyRequest& WithResourceHandlingOption(Aws::String&& value) { SetResourceHandlingOption(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& WithResourceHandlingOption(const char* value) { SetResourceHandlingOption(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Use this only when paginating results to indicate the maximum number of items
     * you want in the response. If additional items exist beyond the maximum you
     * specify, the <code>IsTruncated</code> response element is <code>true</code>.</p>
     * <p>If you do not include this parameter, the number of items defaults to 100.
     * Note that IAM might return fewer results, even when there are more results
     * available. In that case, the <code>IsTruncated</code> response element returns
     * <code>true</code>, and <code>Marker</code> contains a value to include in the
     * subsequent call that tells the service where to continue from.</p>
     */
    inline int GetMaxItems() const{ return m_maxItems; }
    inline bool MaxItemsHasBeenSet() const { return m_maxItemsHasBeenSet; }
    inline void SetMaxItems(int value) { m_maxItemsHasBeenSet = true; m_maxItems = value; }
    inline SimulateCustomPolicyRequest& WithMaxItems(int value) { SetMaxItems(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Use this parameter only when paginating results and only after you receive a
     * response indicating that the results are truncated. Set it to the value of the
     * <code>Marker</code> element in the response that you received to indicate where
     * the next call should start.</p>
     */
    inline const Aws::String& GetMarker() const{ return m_marker; }
    inline bool MarkerHasBeenSet() const { return m_markerHasBeenSet; }
    inline void SetMarker(const Aws::String& value) { m_markerHasBeenSet = true; m_marker = value; }
    inline void SetMarker(Aws::String&& value) { m_markerHasBeenSet = true; m_marker = std::move(value); }
    inline void SetMarker(const char* value) { m_markerHasBeenSet = true; m_marker.assign(value); }
    inline SimulateCustomPolicyRequest& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline SimulateCustomPolicyRequest& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline SimulateCustomPolicyRequest& WithMarker(const char* value) { SetMarker(value); return *this;}
    ///@}
  private:

    Aws::Vector<Aws::String> m_policyInputList;
    bool m_policyInputListHasBeenSet = false;

    Aws::Vector<Aws::String> m_permissionsBoundaryPolicyInputList;
    bool m_permissionsBoundaryPolicyInputListHasBeenSet = false;

    Aws::Vector<Aws::String> m_actionNames;
    bool m_actionNamesHasBeenSet = false;

    Aws::Vector<Aws::String> m_resourceArns;
    bool m_resourceArnsHasBeenSet = false;

    Aws::String m_resourcePolicy;
    bool m_resourcePolicyHasBeenSet = false;

    Aws::String m_resourceOwner;
    bool m_resourceOwnerHasBeenSet = false;

    Aws::String m_callerArn;
    bool m_callerArnHasBeenSet = false;

    Aws::Vector<ContextEntry> m_contextEntries;
    bool m_contextEntriesHasBeenSet = false;

    Aws::String m_resourceHandlingOption;
    bool m_resourceHandlingOptionHasBeenSet = false;

    int m_maxItems;
    bool m_maxItemsHasBeenSet = false;

    Aws::String m_marker;
    bool m_markerHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
