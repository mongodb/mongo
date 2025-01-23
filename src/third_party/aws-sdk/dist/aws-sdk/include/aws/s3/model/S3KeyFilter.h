/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/FilterRule.h>
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
   * <p>A container for object key name prefix and suffix filtering
   * rules.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/S3KeyFilter">AWS API
   * Reference</a></p>
   */
  class S3KeyFilter
  {
  public:
    AWS_S3_API S3KeyFilter();
    AWS_S3_API S3KeyFilter(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API S3KeyFilter& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    
    inline const Aws::Vector<FilterRule>& GetFilterRules() const{ return m_filterRules; }
    inline bool FilterRulesHasBeenSet() const { return m_filterRulesHasBeenSet; }
    inline void SetFilterRules(const Aws::Vector<FilterRule>& value) { m_filterRulesHasBeenSet = true; m_filterRules = value; }
    inline void SetFilterRules(Aws::Vector<FilterRule>&& value) { m_filterRulesHasBeenSet = true; m_filterRules = std::move(value); }
    inline S3KeyFilter& WithFilterRules(const Aws::Vector<FilterRule>& value) { SetFilterRules(value); return *this;}
    inline S3KeyFilter& WithFilterRules(Aws::Vector<FilterRule>&& value) { SetFilterRules(std::move(value)); return *this;}
    inline S3KeyFilter& AddFilterRules(const FilterRule& value) { m_filterRulesHasBeenSet = true; m_filterRules.push_back(value); return *this; }
    inline S3KeyFilter& AddFilterRules(FilterRule&& value) { m_filterRulesHasBeenSet = true; m_filterRules.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::Vector<FilterRule> m_filterRules;
    bool m_filterRulesHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
