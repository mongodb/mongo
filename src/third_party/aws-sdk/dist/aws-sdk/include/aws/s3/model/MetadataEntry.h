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
   * <p>A metadata key-value pair to store with an object.</p><p><h3>See Also:</h3>  
   * <a href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/MetadataEntry">AWS
   * API Reference</a></p>
   */
  class MetadataEntry
  {
  public:
    AWS_S3_API MetadataEntry();
    AWS_S3_API MetadataEntry(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API MetadataEntry& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Name of the object.</p>
     */
    inline const Aws::String& GetName() const{ return m_name; }
    inline bool NameHasBeenSet() const { return m_nameHasBeenSet; }
    inline void SetName(const Aws::String& value) { m_nameHasBeenSet = true; m_name = value; }
    inline void SetName(Aws::String&& value) { m_nameHasBeenSet = true; m_name = std::move(value); }
    inline void SetName(const char* value) { m_nameHasBeenSet = true; m_name.assign(value); }
    inline MetadataEntry& WithName(const Aws::String& value) { SetName(value); return *this;}
    inline MetadataEntry& WithName(Aws::String&& value) { SetName(std::move(value)); return *this;}
    inline MetadataEntry& WithName(const char* value) { SetName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Value of the object.</p>
     */
    inline const Aws::String& GetValue() const{ return m_value; }
    inline bool ValueHasBeenSet() const { return m_valueHasBeenSet; }
    inline void SetValue(const Aws::String& value) { m_valueHasBeenSet = true; m_value = value; }
    inline void SetValue(Aws::String&& value) { m_valueHasBeenSet = true; m_value = std::move(value); }
    inline void SetValue(const char* value) { m_valueHasBeenSet = true; m_value.assign(value); }
    inline MetadataEntry& WithValue(const Aws::String& value) { SetValue(value); return *this;}
    inline MetadataEntry& WithValue(Aws::String&& value) { SetValue(std::move(value)); return *this;}
    inline MetadataEntry& WithValue(const char* value) { SetValue(value); return *this;}
    ///@}
  private:

    Aws::String m_name;
    bool m_nameHasBeenSet = false;

    Aws::String m_value;
    bool m_valueHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
