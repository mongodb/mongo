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
   * <p>Specifies an inventory filter. The inventory only includes objects that meet
   * the filter's criteria.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/InventoryFilter">AWS
   * API Reference</a></p>
   */
  class InventoryFilter
  {
  public:
    AWS_S3_API InventoryFilter();
    AWS_S3_API InventoryFilter(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API InventoryFilter& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The prefix that an object must have to be included in the inventory
     * results.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline InventoryFilter& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline InventoryFilter& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline InventoryFilter& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}
  private:

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
