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
  class GetServiceLinkedRoleDeletionStatusRequest : public IAMRequest
  {
  public:
    AWS_IAM_API GetServiceLinkedRoleDeletionStatusRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetServiceLinkedRoleDeletionStatus"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The deletion task identifier. This identifier is returned by the
     * <a>DeleteServiceLinkedRole</a> operation in the format
     * <code>task/aws-service-role/&lt;service-principal-name&gt;/&lt;role-name&gt;/&lt;task-uuid&gt;</code>.</p>
     */
    inline const Aws::String& GetDeletionTaskId() const{ return m_deletionTaskId; }
    inline bool DeletionTaskIdHasBeenSet() const { return m_deletionTaskIdHasBeenSet; }
    inline void SetDeletionTaskId(const Aws::String& value) { m_deletionTaskIdHasBeenSet = true; m_deletionTaskId = value; }
    inline void SetDeletionTaskId(Aws::String&& value) { m_deletionTaskIdHasBeenSet = true; m_deletionTaskId = std::move(value); }
    inline void SetDeletionTaskId(const char* value) { m_deletionTaskIdHasBeenSet = true; m_deletionTaskId.assign(value); }
    inline GetServiceLinkedRoleDeletionStatusRequest& WithDeletionTaskId(const Aws::String& value) { SetDeletionTaskId(value); return *this;}
    inline GetServiceLinkedRoleDeletionStatusRequest& WithDeletionTaskId(Aws::String&& value) { SetDeletionTaskId(std::move(value)); return *this;}
    inline GetServiceLinkedRoleDeletionStatusRequest& WithDeletionTaskId(const char* value) { SetDeletionTaskId(value); return *this;}
    ///@}
  private:

    Aws::String m_deletionTaskId;
    bool m_deletionTaskIdHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
