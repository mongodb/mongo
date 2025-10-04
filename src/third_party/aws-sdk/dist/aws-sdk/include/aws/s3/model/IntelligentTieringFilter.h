/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/Tag.h>
#include <aws/s3/model/IntelligentTieringAndOperator.h>
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
   * <p>The <code>Filter</code> is used to identify objects that the S3
   * Intelligent-Tiering configuration applies to.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/IntelligentTieringFilter">AWS
   * API Reference</a></p>
   */
  class IntelligentTieringFilter
  {
  public:
    AWS_S3_API IntelligentTieringFilter();
    AWS_S3_API IntelligentTieringFilter(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API IntelligentTieringFilter& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

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
    inline IntelligentTieringFilter& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline IntelligentTieringFilter& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline IntelligentTieringFilter& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    
    inline const Tag& GetTag() const{ return m_tag; }
    inline bool TagHasBeenSet() const { return m_tagHasBeenSet; }
    inline void SetTag(const Tag& value) { m_tagHasBeenSet = true; m_tag = value; }
    inline void SetTag(Tag&& value) { m_tagHasBeenSet = true; m_tag = std::move(value); }
    inline IntelligentTieringFilter& WithTag(const Tag& value) { SetTag(value); return *this;}
    inline IntelligentTieringFilter& WithTag(Tag&& value) { SetTag(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A conjunction (logical AND) of predicates, which is used in evaluating a
     * metrics filter. The operator must have at least two predicates, and an object
     * must match all of the predicates in order for the filter to apply.</p>
     */
    inline const IntelligentTieringAndOperator& GetAnd() const{ return m_and; }
    inline bool AndHasBeenSet() const { return m_andHasBeenSet; }
    inline void SetAnd(const IntelligentTieringAndOperator& value) { m_andHasBeenSet = true; m_and = value; }
    inline void SetAnd(IntelligentTieringAndOperator&& value) { m_andHasBeenSet = true; m_and = std::move(value); }
    inline IntelligentTieringFilter& WithAnd(const IntelligentTieringAndOperator& value) { SetAnd(value); return *this;}
    inline IntelligentTieringFilter& WithAnd(IntelligentTieringAndOperator&& value) { SetAnd(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    Tag m_tag;
    bool m_tagHasBeenSet = false;

    IntelligentTieringAndOperator m_and;
    bool m_andHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
