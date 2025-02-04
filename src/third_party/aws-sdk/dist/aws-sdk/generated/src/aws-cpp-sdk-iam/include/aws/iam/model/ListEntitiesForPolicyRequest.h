/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/EntityType.h>
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
  class ListEntitiesForPolicyRequest : public IAMRequest
  {
  public:
    AWS_IAM_API ListEntitiesForPolicyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListEntitiesForPolicy"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the IAM policy for which you want the
     * versions.</p> <p>For more information about ARNs, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">Amazon
     * Resource Names (ARNs)</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>
     */
    inline const Aws::String& GetPolicyArn() const{ return m_policyArn; }
    inline bool PolicyArnHasBeenSet() const { return m_policyArnHasBeenSet; }
    inline void SetPolicyArn(const Aws::String& value) { m_policyArnHasBeenSet = true; m_policyArn = value; }
    inline void SetPolicyArn(Aws::String&& value) { m_policyArnHasBeenSet = true; m_policyArn = std::move(value); }
    inline void SetPolicyArn(const char* value) { m_policyArnHasBeenSet = true; m_policyArn.assign(value); }
    inline ListEntitiesForPolicyRequest& WithPolicyArn(const Aws::String& value) { SetPolicyArn(value); return *this;}
    inline ListEntitiesForPolicyRequest& WithPolicyArn(Aws::String&& value) { SetPolicyArn(std::move(value)); return *this;}
    inline ListEntitiesForPolicyRequest& WithPolicyArn(const char* value) { SetPolicyArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The entity type to use for filtering the results.</p> <p>For example, when
     * <code>EntityFilter</code> is <code>Role</code>, only the roles that are attached
     * to the specified policy are returned. This parameter is optional. If it is not
     * included, all attached entities (users, groups, and roles) are returned. The
     * argument for this parameter must be one of the valid values listed below.</p>
     */
    inline const EntityType& GetEntityFilter() const{ return m_entityFilter; }
    inline bool EntityFilterHasBeenSet() const { return m_entityFilterHasBeenSet; }
    inline void SetEntityFilter(const EntityType& value) { m_entityFilterHasBeenSet = true; m_entityFilter = value; }
    inline void SetEntityFilter(EntityType&& value) { m_entityFilterHasBeenSet = true; m_entityFilter = std::move(value); }
    inline ListEntitiesForPolicyRequest& WithEntityFilter(const EntityType& value) { SetEntityFilter(value); return *this;}
    inline ListEntitiesForPolicyRequest& WithEntityFilter(EntityType&& value) { SetEntityFilter(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The path prefix for filtering the results. This parameter is optional. If it
     * is not included, it defaults to a slash (/), listing all entities.</p> <p>This
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
    inline ListEntitiesForPolicyRequest& WithPathPrefix(const Aws::String& value) { SetPathPrefix(value); return *this;}
    inline ListEntitiesForPolicyRequest& WithPathPrefix(Aws::String&& value) { SetPathPrefix(std::move(value)); return *this;}
    inline ListEntitiesForPolicyRequest& WithPathPrefix(const char* value) { SetPathPrefix(value); return *this;}
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
    inline ListEntitiesForPolicyRequest& WithPolicyUsageFilter(const PolicyUsageType& value) { SetPolicyUsageFilter(value); return *this;}
    inline ListEntitiesForPolicyRequest& WithPolicyUsageFilter(PolicyUsageType&& value) { SetPolicyUsageFilter(std::move(value)); return *this;}
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
    inline ListEntitiesForPolicyRequest& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline ListEntitiesForPolicyRequest& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline ListEntitiesForPolicyRequest& WithMarker(const char* value) { SetMarker(value); return *this;}
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
    inline ListEntitiesForPolicyRequest& WithMaxItems(int value) { SetMaxItems(value); return *this;}
    ///@}
  private:

    Aws::String m_policyArn;
    bool m_policyArnHasBeenSet = false;

    EntityType m_entityFilter;
    bool m_entityFilterHasBeenSet = false;

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
