/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/Tag.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class TagInstanceProfileRequest : public IAMRequest
  {
  public:
    AWS_IAM_API TagInstanceProfileRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "TagInstanceProfile"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the IAM instance profile to which you want to add tags.</p>
     * <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * consisting of upper and lowercase alphanumeric characters with no spaces. You
     * can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetInstanceProfileName() const{ return m_instanceProfileName; }
    inline bool InstanceProfileNameHasBeenSet() const { return m_instanceProfileNameHasBeenSet; }
    inline void SetInstanceProfileName(const Aws::String& value) { m_instanceProfileNameHasBeenSet = true; m_instanceProfileName = value; }
    inline void SetInstanceProfileName(Aws::String&& value) { m_instanceProfileNameHasBeenSet = true; m_instanceProfileName = std::move(value); }
    inline void SetInstanceProfileName(const char* value) { m_instanceProfileNameHasBeenSet = true; m_instanceProfileName.assign(value); }
    inline TagInstanceProfileRequest& WithInstanceProfileName(const Aws::String& value) { SetInstanceProfileName(value); return *this;}
    inline TagInstanceProfileRequest& WithInstanceProfileName(Aws::String&& value) { SetInstanceProfileName(std::move(value)); return *this;}
    inline TagInstanceProfileRequest& WithInstanceProfileName(const char* value) { SetInstanceProfileName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The list of tags that you want to attach to the IAM instance profile. Each
     * tag consists of a key name and an associated value.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline TagInstanceProfileRequest& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline TagInstanceProfileRequest& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline TagInstanceProfileRequest& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline TagInstanceProfileRequest& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_instanceProfileName;
    bool m_instanceProfileNameHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
