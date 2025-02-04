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
  class CreateVirtualMFADeviceRequest : public IAMRequest
  {
  public:
    AWS_IAM_API CreateVirtualMFADeviceRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateVirtualMFADevice"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p> The path for the virtual MFA device. For more information about paths, see
     * <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p> <p>This parameter is optional.
     * If it is not included, it defaults to a slash (/).</p> <p>This parameter allows
     * (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a
     * string of characters consisting of either a forward slash (/) by itself or a
     * string that must begin and end with forward slashes. In addition, it can contain
     * any ASCII character from the ! (<code>\u0021</code>) through the DEL character
     * (<code>\u007F</code>), including most punctuation characters, digits, and upper
     * and lowercased letters.</p>
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline CreateVirtualMFADeviceRequest& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline CreateVirtualMFADeviceRequest& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline CreateVirtualMFADeviceRequest& WithPath(const char* value) { SetPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the virtual MFA device, which must be unique. Use with path to
     * uniquely identify a virtual MFA device.</p> <p>This parameter allows (through
     * its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of
     * characters consisting of upper and lowercase alphanumeric characters with no
     * spaces. You can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetVirtualMFADeviceName() const{ return m_virtualMFADeviceName; }
    inline bool VirtualMFADeviceNameHasBeenSet() const { return m_virtualMFADeviceNameHasBeenSet; }
    inline void SetVirtualMFADeviceName(const Aws::String& value) { m_virtualMFADeviceNameHasBeenSet = true; m_virtualMFADeviceName = value; }
    inline void SetVirtualMFADeviceName(Aws::String&& value) { m_virtualMFADeviceNameHasBeenSet = true; m_virtualMFADeviceName = std::move(value); }
    inline void SetVirtualMFADeviceName(const char* value) { m_virtualMFADeviceNameHasBeenSet = true; m_virtualMFADeviceName.assign(value); }
    inline CreateVirtualMFADeviceRequest& WithVirtualMFADeviceName(const Aws::String& value) { SetVirtualMFADeviceName(value); return *this;}
    inline CreateVirtualMFADeviceRequest& WithVirtualMFADeviceName(Aws::String&& value) { SetVirtualMFADeviceName(std::move(value)); return *this;}
    inline CreateVirtualMFADeviceRequest& WithVirtualMFADeviceName(const char* value) { SetVirtualMFADeviceName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that you want to attach to the new IAM virtual MFA device.
     * Each tag consists of a key name and an associated value. For more information
     * about tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>  <p>If any one of the tags
     * is invalid or if you exceed the allowed maximum number of tags, then the entire
     * request fails and the resource is not created.</p> 
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline CreateVirtualMFADeviceRequest& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline CreateVirtualMFADeviceRequest& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline CreateVirtualMFADeviceRequest& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline CreateVirtualMFADeviceRequest& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_path;
    bool m_pathHasBeenSet = false;

    Aws::String m_virtualMFADeviceName;
    bool m_virtualMFADeviceNameHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
