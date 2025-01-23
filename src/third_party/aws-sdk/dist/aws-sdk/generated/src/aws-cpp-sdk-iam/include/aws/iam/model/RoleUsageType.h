/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
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
   * <p>An object that contains details about how a service-linked role is used, if
   * that information is returned by the service.</p> <p>This data type is used as a
   * response element in the <a>GetServiceLinkedRoleDeletionStatus</a>
   * operation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/RoleUsageType">AWS
   * API Reference</a></p>
   */
  class RoleUsageType
  {
  public:
    AWS_IAM_API RoleUsageType();
    AWS_IAM_API RoleUsageType(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API RoleUsageType& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The name of the Region where the service-linked role is being used.</p>
     */
    inline const Aws::String& GetRegion() const{ return m_region; }
    inline bool RegionHasBeenSet() const { return m_regionHasBeenSet; }
    inline void SetRegion(const Aws::String& value) { m_regionHasBeenSet = true; m_region = value; }
    inline void SetRegion(Aws::String&& value) { m_regionHasBeenSet = true; m_region = std::move(value); }
    inline void SetRegion(const char* value) { m_regionHasBeenSet = true; m_region.assign(value); }
    inline RoleUsageType& WithRegion(const Aws::String& value) { SetRegion(value); return *this;}
    inline RoleUsageType& WithRegion(Aws::String&& value) { SetRegion(std::move(value)); return *this;}
    inline RoleUsageType& WithRegion(const char* value) { SetRegion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the resource that is using the service-linked role.</p>
     */
    inline const Aws::Vector<Aws::String>& GetResources() const{ return m_resources; }
    inline bool ResourcesHasBeenSet() const { return m_resourcesHasBeenSet; }
    inline void SetResources(const Aws::Vector<Aws::String>& value) { m_resourcesHasBeenSet = true; m_resources = value; }
    inline void SetResources(Aws::Vector<Aws::String>&& value) { m_resourcesHasBeenSet = true; m_resources = std::move(value); }
    inline RoleUsageType& WithResources(const Aws::Vector<Aws::String>& value) { SetResources(value); return *this;}
    inline RoleUsageType& WithResources(Aws::Vector<Aws::String>&& value) { SetResources(std::move(value)); return *this;}
    inline RoleUsageType& AddResources(const Aws::String& value) { m_resourcesHasBeenSet = true; m_resources.push_back(value); return *this; }
    inline RoleUsageType& AddResources(Aws::String&& value) { m_resourcesHasBeenSet = true; m_resources.push_back(std::move(value)); return *this; }
    inline RoleUsageType& AddResources(const char* value) { m_resourcesHasBeenSet = true; m_resources.push_back(value); return *this; }
    ///@}
  private:

    Aws::String m_region;
    bool m_regionHasBeenSet = false;

    Aws::Vector<Aws::String> m_resources;
    bool m_resourcesHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
