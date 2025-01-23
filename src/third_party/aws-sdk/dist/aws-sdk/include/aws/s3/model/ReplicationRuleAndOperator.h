/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/Tag.h>
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
   * <p>A container for specifying rule filters. The filters determine the subset of
   * objects to which the rule applies. This element is required only if you specify
   * more than one filter. </p> <p>For example:</p> <ul> <li> <p>If you specify both
   * a <code>Prefix</code> and a <code>Tag</code> filter, wrap these filters in an
   * <code>And</code> tag. </p> </li> <li> <p>If you specify a filter based on
   * multiple tags, wrap the <code>Tag</code> elements in an <code>And</code>
   * tag.</p> </li> </ul><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ReplicationRuleAndOperator">AWS
   * API Reference</a></p>
   */
  class ReplicationRuleAndOperator
  {
  public:
    AWS_S3_API ReplicationRuleAndOperator();
    AWS_S3_API ReplicationRuleAndOperator(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ReplicationRuleAndOperator& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>An object key name prefix that identifies the subset of objects to which the
     * rule applies.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline ReplicationRuleAndOperator& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ReplicationRuleAndOperator& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ReplicationRuleAndOperator& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An array of tags containing key and value pairs.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline ReplicationRuleAndOperator& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline ReplicationRuleAndOperator& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline ReplicationRuleAndOperator& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline ReplicationRuleAndOperator& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
