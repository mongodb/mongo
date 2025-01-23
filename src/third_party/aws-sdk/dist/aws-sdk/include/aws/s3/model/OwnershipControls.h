/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/OwnershipControlsRule.h>
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
   * <p>The container element for a bucket's ownership controls.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/OwnershipControls">AWS
   * API Reference</a></p>
   */
  class OwnershipControls
  {
  public:
    AWS_S3_API OwnershipControls();
    AWS_S3_API OwnershipControls(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API OwnershipControls& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The container element for an ownership control rule.</p>
     */
    inline const Aws::Vector<OwnershipControlsRule>& GetRules() const{ return m_rules; }
    inline bool RulesHasBeenSet() const { return m_rulesHasBeenSet; }
    inline void SetRules(const Aws::Vector<OwnershipControlsRule>& value) { m_rulesHasBeenSet = true; m_rules = value; }
    inline void SetRules(Aws::Vector<OwnershipControlsRule>&& value) { m_rulesHasBeenSet = true; m_rules = std::move(value); }
    inline OwnershipControls& WithRules(const Aws::Vector<OwnershipControlsRule>& value) { SetRules(value); return *this;}
    inline OwnershipControls& WithRules(Aws::Vector<OwnershipControlsRule>&& value) { SetRules(std::move(value)); return *this;}
    inline OwnershipControls& AddRules(const OwnershipControlsRule& value) { m_rulesHasBeenSet = true; m_rules.push_back(value); return *this; }
    inline OwnershipControls& AddRules(OwnershipControlsRule&& value) { m_rulesHasBeenSet = true; m_rules.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::Vector<OwnershipControlsRule> m_rules;
    bool m_rulesHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
