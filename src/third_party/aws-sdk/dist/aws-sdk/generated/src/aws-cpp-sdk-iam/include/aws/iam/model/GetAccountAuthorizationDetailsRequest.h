/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/EntityType.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class GetAccountAuthorizationDetailsRequest : public IAMRequest
  {
  public:
    AWS_IAM_API GetAccountAuthorizationDetailsRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetAccountAuthorizationDetails"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>A list of entity types used to filter the results. Only the entities that
     * match the types you specify are included in the output. Use the value
     * <code>LocalManagedPolicy</code> to include customer managed policies.</p> <p>The
     * format for this parameter is a comma-separated (if more than one) list of
     * strings. Each string value in the list must be one of the valid values listed
     * below.</p>
     */
    inline const Aws::Vector<EntityType>& GetFilter() const{ return m_filter; }
    inline bool FilterHasBeenSet() const { return m_filterHasBeenSet; }
    inline void SetFilter(const Aws::Vector<EntityType>& value) { m_filterHasBeenSet = true; m_filter = value; }
    inline void SetFilter(Aws::Vector<EntityType>&& value) { m_filterHasBeenSet = true; m_filter = std::move(value); }
    inline GetAccountAuthorizationDetailsRequest& WithFilter(const Aws::Vector<EntityType>& value) { SetFilter(value); return *this;}
    inline GetAccountAuthorizationDetailsRequest& WithFilter(Aws::Vector<EntityType>&& value) { SetFilter(std::move(value)); return *this;}
    inline GetAccountAuthorizationDetailsRequest& AddFilter(const EntityType& value) { m_filterHasBeenSet = true; m_filter.push_back(value); return *this; }
    inline GetAccountAuthorizationDetailsRequest& AddFilter(EntityType&& value) { m_filterHasBeenSet = true; m_filter.push_back(std::move(value)); return *this; }
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
    inline GetAccountAuthorizationDetailsRequest& WithMaxItems(int value) { SetMaxItems(value); return *this;}
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
    inline GetAccountAuthorizationDetailsRequest& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline GetAccountAuthorizationDetailsRequest& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline GetAccountAuthorizationDetailsRequest& WithMarker(const char* value) { SetMarker(value); return *this;}
    ///@}
  private:

    Aws::Vector<EntityType> m_filter;
    bool m_filterHasBeenSet = false;

    int m_maxItems;
    bool m_maxItemsHasBeenSet = false;

    Aws::String m_marker;
    bool m_markerHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
