/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/iam/model/PolicyScopeType.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/PolicyUsageType.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class ListPoliciesRequest : public IAMRequest
  {
  public:
    AWS_IAM_API ListPoliciesRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListPolicies"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The scope to use for filtering the results.</p> <p>To list only Amazon Web
     * Services managed policies, set <code>Scope</code> to <code>AWS</code>. To list
     * only the customer managed policies in your Amazon Web Services account, set
     * <code>Scope</code> to <code>Local</code>.</p> <p>This parameter is optional. If
     * it is not included, or if it is set to <code>All</code>, all policies are
     * returned.</p>
     */
    inline const PolicyScopeType& GetScope() const{ return m_scope; }
    inline bool ScopeHasBeenSet() const { return m_scopeHasBeenSet; }
    inline void SetScope(const PolicyScopeType& value) { m_scopeHasBeenSet = true; m_scope = value; }
    inline void SetScope(PolicyScopeType&& value) { m_scopeHasBeenSet = true; m_scope = std::move(value); }
    inline ListPoliciesRequest& WithScope(const PolicyScopeType& value) { SetScope(value); return *this;}
    inline ListPoliciesRequest& WithScope(PolicyScopeType&& value) { SetScope(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A flag to filter the results to only the attached policies.</p> <p>When
     * <code>OnlyAttached</code> is <code>true</code>, the returned list contains only
     * the policies that are attached to an IAM user, group, or role. When
     * <code>OnlyAttached</code> is <code>false</code>, or when the parameter is not
     * included, all policies are returned.</p>
     */
    inline bool GetOnlyAttached() const{ return m_onlyAttached; }
    inline bool OnlyAttachedHasBeenSet() const { return m_onlyAttachedHasBeenSet; }
    inline void SetOnlyAttached(bool value) { m_onlyAttachedHasBeenSet = true; m_onlyAttached = value; }
    inline ListPoliciesRequest& WithOnlyAttached(bool value) { SetOnlyAttached(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The path prefix for filtering the results. This parameter is optional. If it
     * is not included, it defaults to a slash (/), listing all policies. This
     * parameter allows (through its <a href="http://wikipedia.org/wiki/regex">regex
     * pattern</a>) a string of characters consisting of either a forward slash (/) by
     * itself or a string that must begin and end with forward slashes. In addition, it
     * can contain any ASCII character from the ! (<code>\u0021</code>) through the DEL
     * character (<code>\u007F</code>), including most punctuation characters, digits,
     * and upper and lowercased letters.</p>
     */
    inline const Aws::String& GetPathPrefix() const{ return m_pathPrefix; }
    inline bool PathPrefixHasBeenSet() const { return m_pathPrefixHasBeenSet; }
    inline void SetPathPrefix(const Aws::String& value) { m_pathPrefixHasBeenSet = true; m_pathPrefix = value; }
    inline void SetPathPrefix(Aws::String&& value) { m_pathPrefixHasBeenSet = true; m_pathPrefix = std::move(value); }
    inline void SetPathPrefix(const char* value) { m_pathPrefixHasBeenSet = true; m_pathPrefix.assign(value); }
    inline ListPoliciesRequest& WithPathPrefix(const Aws::String& value) { SetPathPrefix(value); return *this;}
    inline ListPoliciesRequest& WithPathPrefix(Aws::String&& value) { SetPathPrefix(std::move(value)); return *this;}
    inline ListPoliciesRequest& WithPathPrefix(const char* value) { SetPathPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The policy usage method to use for filtering the results.</p> <p>To list only
     * permissions policies,
     * set <code>PolicyUsageFilter</code> to <code>PermissionsPolicy</code>. To list
     * only the policies used to set permissions boundaries, set the value
     * to <code>PermissionsBoundary</code>.</p> <p>This parameter is optional. If it is
     * not included, all policies are returned. </p>
     */
    inline const PolicyUsageType& GetPolicyUsageFilter() const{ return m_policyUsageFilter; }
    inline bool PolicyUsageFilterHasBeenSet() const { return m_policyUsageFilterHasBeenSet; }
    inline void SetPolicyUsageFilter(const PolicyUsageType& value) { m_policyUsageFilterHasBeenSet = true; m_policyUsageFilter = value; }
    inline void SetPolicyUsageFilter(PolicyUsageType&& value) { m_policyUsageFilterHasBeenSet = true; m_policyUsageFilter = std::move(value); }
    inline ListPoliciesRequest& WithPolicyUsageFilter(const PolicyUsageType& value) { SetPolicyUsageFilter(value); return *this;}
    inline ListPoliciesRequest& WithPolicyUsageFilter(PolicyUsageType&& value) { SetPolicyUsageFilter(std::move(value)); return *this;}
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
    inline ListPoliciesRequest& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline ListPoliciesRequest& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline ListPoliciesRequest& WithMarker(const char* value) { SetMarker(value); return *this;}
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
    inline ListPoliciesRequest& WithMaxItems(int value) { SetMaxItems(value); return *this;}
    ///@}
  private:

    PolicyScopeType m_scope;
    bool m_scopeHasBeenSet = false;

    bool m_onlyAttached;
    bool m_onlyAttachedHasBeenSet = false;

    Aws::String m_pathPrefix;
    bool m_pathPrefixHasBeenSet = false;

    PolicyUsageType m_policyUsageFilter;
    bool m_policyUsageFilterHasBeenSet = false;

    Aws::String m_marker;
    bool m_markerHasBeenSet = false;

    int m_maxItems;
    bool m_maxItemsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
