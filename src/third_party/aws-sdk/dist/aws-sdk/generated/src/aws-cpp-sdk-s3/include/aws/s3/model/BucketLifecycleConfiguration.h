/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/LifecycleRule.h>
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
   * <p>Specifies the lifecycle configuration for objects in an Amazon S3 bucket. For
   * more information, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lifecycle-mgmt.html">Object
   * Lifecycle Management</a> in the <i>Amazon S3 User Guide</i>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/BucketLifecycleConfiguration">AWS
   * API Reference</a></p>
   */
  class BucketLifecycleConfiguration
  {
  public:
    AWS_S3_API BucketLifecycleConfiguration();
    AWS_S3_API BucketLifecycleConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API BucketLifecycleConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>A lifecycle rule for individual objects in an Amazon S3 bucket.</p>
     */
    inline const Aws::Vector<LifecycleRule>& GetRules() const{ return m_rules; }
    inline bool RulesHasBeenSet() const { return m_rulesHasBeenSet; }
    inline void SetRules(const Aws::Vector<LifecycleRule>& value) { m_rulesHasBeenSet = true; m_rules = value; }
    inline void SetRules(Aws::Vector<LifecycleRule>&& value) { m_rulesHasBeenSet = true; m_rules = std::move(value); }
    inline BucketLifecycleConfiguration& WithRules(const Aws::Vector<LifecycleRule>& value) { SetRules(value); return *this;}
    inline BucketLifecycleConfiguration& WithRules(Aws::Vector<LifecycleRule>&& value) { SetRules(std::move(value)); return *this;}
    inline BucketLifecycleConfiguration& AddRules(const LifecycleRule& value) { m_rulesHasBeenSet = true; m_rules.push_back(value); return *this; }
    inline BucketLifecycleConfiguration& AddRules(LifecycleRule&& value) { m_rulesHasBeenSet = true; m_rules.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::Vector<LifecycleRule> m_rules;
    bool m_rulesHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
