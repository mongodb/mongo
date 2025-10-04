/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/FilterRuleName.h>
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
   * <p>Specifies the Amazon S3 object key name to filter on. An object key name is
   * the name assigned to an object in your Amazon S3 bucket. You specify whether to
   * filter on the suffix or prefix of the object key name. A prefix is a specific
   * string of characters at the beginning of an object key name, which you can use
   * to organize objects. For example, you can start the key names of related objects
   * with a prefix, such as <code>2023-</code> or <code>engineering/</code>. Then,
   * you can use <code>FilterRule</code> to find objects in a bucket with key names
   * that have the same prefix. A suffix is similar to a prefix, but it is at the end
   * of the object key name instead of at the beginning.</p><p><h3>See Also:</h3>  
   * <a href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/FilterRule">AWS
   * API Reference</a></p>
   */
  class FilterRule
  {
  public:
    AWS_S3_API FilterRule();
    AWS_S3_API FilterRule(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API FilterRule& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The object key name prefix or suffix identifying one or more objects to which
     * the filtering rule applies. The maximum length is 1,024 characters. Overlapping
     * prefixes and suffixes are not supported. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/NotificationHowTo.html">Configuring
     * Event Notifications</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const FilterRuleName& GetName() const{ return m_name; }
    inline bool NameHasBeenSet() const { return m_nameHasBeenSet; }
    inline void SetName(const FilterRuleName& value) { m_nameHasBeenSet = true; m_name = value; }
    inline void SetName(FilterRuleName&& value) { m_nameHasBeenSet = true; m_name = std::move(value); }
    inline FilterRule& WithName(const FilterRuleName& value) { SetName(value); return *this;}
    inline FilterRule& WithName(FilterRuleName&& value) { SetName(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The value that the filter searches for in object key names.</p>
     */
    inline const Aws::String& GetValue() const{ return m_value; }
    inline bool ValueHasBeenSet() const { return m_valueHasBeenSet; }
    inline void SetValue(const Aws::String& value) { m_valueHasBeenSet = true; m_value = value; }
    inline void SetValue(Aws::String&& value) { m_valueHasBeenSet = true; m_value = std::move(value); }
    inline void SetValue(const char* value) { m_valueHasBeenSet = true; m_value.assign(value); }
    inline FilterRule& WithValue(const Aws::String& value) { SetValue(value); return *this;}
    inline FilterRule& WithValue(Aws::String&& value) { SetValue(std::move(value)); return *this;}
    inline FilterRule& WithValue(const char* value) { SetValue(value); return *this;}
    ///@}
  private:

    FilterRuleName m_name;
    bool m_nameHasBeenSet = false;

    Aws::String m_value;
    bool m_valueHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
