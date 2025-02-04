/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/Tag.h>
#include <aws/s3/model/ReplicationRuleAndOperator.h>
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
   * <p>A filter that identifies the subset of objects to which the replication rule
   * applies. A <code>Filter</code> must specify exactly one <code>Prefix</code>,
   * <code>Tag</code>, or an <code>And</code> child element.</p><p><h3>See Also:</h3>
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ReplicationRuleFilter">AWS
   * API Reference</a></p>
   */
  class ReplicationRuleFilter
  {
  public:
    AWS_S3_API ReplicationRuleFilter();
    AWS_S3_API ReplicationRuleFilter(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ReplicationRuleFilter& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>An object key name prefix that identifies the subset of objects to which the
     * rule applies.</p>  <p>Replacement must be made for object keys
     * containing special characters (such as carriage returns) when using XML
     * requests. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html#object-key-xml-related-constraints">
     * XML related object key constraints</a>.</p> 
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline ReplicationRuleFilter& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ReplicationRuleFilter& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ReplicationRuleFilter& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A container for specifying a tag key and value. </p> <p>The rule applies only
     * to objects that have the tag in their tag set.</p>
     */
    inline const Tag& GetTag() const{ return m_tag; }
    inline bool TagHasBeenSet() const { return m_tagHasBeenSet; }
    inline void SetTag(const Tag& value) { m_tagHasBeenSet = true; m_tag = value; }
    inline void SetTag(Tag&& value) { m_tagHasBeenSet = true; m_tag = std::move(value); }
    inline ReplicationRuleFilter& WithTag(const Tag& value) { SetTag(value); return *this;}
    inline ReplicationRuleFilter& WithTag(Tag&& value) { SetTag(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A container for specifying rule filters. The filters determine the subset of
     * objects to which the rule applies. This element is required only if you specify
     * more than one filter. For example: </p> <ul> <li> <p>If you specify both a
     * <code>Prefix</code> and a <code>Tag</code> filter, wrap these filters in an
     * <code>And</code> tag.</p> </li> <li> <p>If you specify a filter based on
     * multiple tags, wrap the <code>Tag</code> elements in an <code>And</code>
     * tag.</p> </li> </ul>
     */
    inline const ReplicationRuleAndOperator& GetAnd() const{ return m_and; }
    inline bool AndHasBeenSet() const { return m_andHasBeenSet; }
    inline void SetAnd(const ReplicationRuleAndOperator& value) { m_andHasBeenSet = true; m_and = value; }
    inline void SetAnd(ReplicationRuleAndOperator&& value) { m_andHasBeenSet = true; m_and = std::move(value); }
    inline ReplicationRuleFilter& WithAnd(const ReplicationRuleAndOperator& value) { SetAnd(value); return *this;}
    inline ReplicationRuleFilter& WithAnd(ReplicationRuleAndOperator&& value) { SetAnd(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    Tag m_tag;
    bool m_tagHasBeenSet = false;

    ReplicationRuleAndOperator m_and;
    bool m_andHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
