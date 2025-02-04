/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class ListAttachedGroupPoliciesRequest : public IAMRequest
  {
  public:
    AWS_IAM_API ListAttachedGroupPoliciesRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListAttachedGroupPolicies"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name (friendly name, not ARN) of the group to list attached policies
     * for.</p> <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * consisting of upper and lowercase alphanumeric characters with no spaces. You
     * can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetGroupName() const{ return m_groupName; }
    inline bool GroupNameHasBeenSet() const { return m_groupNameHasBeenSet; }
    inline void SetGroupName(const Aws::String& value) { m_groupNameHasBeenSet = true; m_groupName = value; }
    inline void SetGroupName(Aws::String&& value) { m_groupNameHasBeenSet = true; m_groupName = std::move(value); }
    inline void SetGroupName(const char* value) { m_groupNameHasBeenSet = true; m_groupName.assign(value); }
    inline ListAttachedGroupPoliciesRequest& WithGroupName(const Aws::String& value) { SetGroupName(value); return *this;}
    inline ListAttachedGroupPoliciesRequest& WithGroupName(Aws::String&& value) { SetGroupName(std::move(value)); return *this;}
    inline ListAttachedGroupPoliciesRequest& WithGroupName(const char* value) { SetGroupName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The path prefix for filtering the results. This parameter is optional. If it
     * is not included, it defaults to a slash (/), listing all policies.</p> <p>This
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
    inline ListAttachedGroupPoliciesRequest& WithPathPrefix(const Aws::String& value) { SetPathPrefix(value); return *this;}
    inline ListAttachedGroupPoliciesRequest& WithPathPrefix(Aws::String&& value) { SetPathPrefix(std::move(value)); return *this;}
    inline ListAttachedGroupPoliciesRequest& WithPathPrefix(const char* value) { SetPathPrefix(value); return *this;}
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
    inline ListAttachedGroupPoliciesRequest& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline ListAttachedGroupPoliciesRequest& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline ListAttachedGroupPoliciesRequest& WithMarker(const char* value) { SetMarker(value); return *this;}
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
    inline ListAttachedGroupPoliciesRequest& WithMaxItems(int value) { SetMaxItems(value); return *this;}
    ///@}
  private:

    Aws::String m_groupName;
    bool m_groupNameHasBeenSet = false;

    Aws::String m_pathPrefix;
    bool m_pathPrefixHasBeenSet = false;

    Aws::String m_marker;
    bool m_markerHasBeenSet = false;

    int m_maxItems;
    bool m_maxItemsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
