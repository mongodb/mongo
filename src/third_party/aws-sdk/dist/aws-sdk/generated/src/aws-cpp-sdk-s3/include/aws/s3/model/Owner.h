/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
namespace S3
{
namespace Model
{

  /**
   * <p>Container for the owner's display name and ID.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Owner">AWS API
   * Reference</a></p>
   */
  class Owner
  {
  public:
    AWS_S3_API Owner();
    AWS_S3_API Owner(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Owner& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Container for the display name of the owner. This value is only supported in
     * the following Amazon Web Services Regions:</p> <ul> <li> <p>US East (N.
     * Virginia)</p> </li> <li> <p>US West (N. California)</p> </li> <li> <p>US West
     * (Oregon)</p> </li> <li> <p>Asia Pacific (Singapore)</p> </li> <li> <p>Asia
     * Pacific (Sydney)</p> </li> <li> <p>Asia Pacific (Tokyo)</p> </li> <li> <p>Europe
     * (Ireland)</p> </li> <li> <p>South America (São Paulo)</p> </li> </ul> 
     * <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetDisplayName() const{ return m_displayName; }
    inline bool DisplayNameHasBeenSet() const { return m_displayNameHasBeenSet; }
    inline void SetDisplayName(const Aws::String& value) { m_displayNameHasBeenSet = true; m_displayName = value; }
    inline void SetDisplayName(Aws::String&& value) { m_displayNameHasBeenSet = true; m_displayName = std::move(value); }
    inline void SetDisplayName(const char* value) { m_displayNameHasBeenSet = true; m_displayName.assign(value); }
    inline Owner& WithDisplayName(const Aws::String& value) { SetDisplayName(value); return *this;}
    inline Owner& WithDisplayName(Aws::String&& value) { SetDisplayName(std::move(value)); return *this;}
    inline Owner& WithDisplayName(const char* value) { SetDisplayName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for the ID of the owner.</p>
     */
    inline const Aws::String& GetID() const{ return m_iD; }
    inline bool IDHasBeenSet() const { return m_iDHasBeenSet; }
    inline void SetID(const Aws::String& value) { m_iDHasBeenSet = true; m_iD = value; }
    inline void SetID(Aws::String&& value) { m_iDHasBeenSet = true; m_iD = std::move(value); }
    inline void SetID(const char* value) { m_iDHasBeenSet = true; m_iD.assign(value); }
    inline Owner& WithID(const Aws::String& value) { SetID(value); return *this;}
    inline Owner& WithID(Aws::String&& value) { SetID(std::move(value)); return *this;}
    inline Owner& WithID(const char* value) { SetID(value); return *this;}
    ///@}
  private:

    Aws::String m_displayName;
    bool m_displayNameHasBeenSet = false;

    Aws::String m_iD;
    bool m_iDHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
