/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/Tag.h>
#include <aws/s3/model/AnalyticsAndOperator.h>
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
   * <p>The filter used to describe a set of objects for analyses. A filter must have
   * exactly one prefix, one tag, or one conjunction (AnalyticsAndOperator). If no
   * filter is provided, all objects will be considered in any
   * analysis.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/AnalyticsFilter">AWS
   * API Reference</a></p>
   */
  class AnalyticsFilter
  {
  public:
    AWS_S3_API AnalyticsFilter();
    AWS_S3_API AnalyticsFilter(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API AnalyticsFilter& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The prefix to use when evaluating an analytics filter.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline AnalyticsFilter& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline AnalyticsFilter& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline AnalyticsFilter& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The tag to use when evaluating an analytics filter.</p>
     */
    inline const Tag& GetTag() const{ return m_tag; }
    inline bool TagHasBeenSet() const { return m_tagHasBeenSet; }
    inline void SetTag(const Tag& value) { m_tagHasBeenSet = true; m_tag = value; }
    inline void SetTag(Tag&& value) { m_tagHasBeenSet = true; m_tag = std::move(value); }
    inline AnalyticsFilter& WithTag(const Tag& value) { SetTag(value); return *this;}
    inline AnalyticsFilter& WithTag(Tag&& value) { SetTag(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A conjunction (logical AND) of predicates, which is used in evaluating an
     * analytics filter. The operator must have at least two predicates.</p>
     */
    inline const AnalyticsAndOperator& GetAnd() const{ return m_and; }
    inline bool AndHasBeenSet() const { return m_andHasBeenSet; }
    inline void SetAnd(const AnalyticsAndOperator& value) { m_andHasBeenSet = true; m_and = value; }
    inline void SetAnd(AnalyticsAndOperator&& value) { m_andHasBeenSet = true; m_and = std::move(value); }
    inline AnalyticsFilter& WithAnd(const AnalyticsAndOperator& value) { SetAnd(value); return *this;}
    inline AnalyticsFilter& WithAnd(AnalyticsAndOperator&& value) { SetAnd(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    Tag m_tag;
    bool m_tagHasBeenSet = false;

    AnalyticsAndOperator m_and;
    bool m_andHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
