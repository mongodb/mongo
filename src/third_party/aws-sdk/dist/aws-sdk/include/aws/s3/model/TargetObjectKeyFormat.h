/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/SimplePrefix.h>
#include <aws/s3/model/PartitionedPrefix.h>
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
   * <p>Amazon S3 key format for log objects. Only one format, PartitionedPrefix or
   * SimplePrefix, is allowed.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/TargetObjectKeyFormat">AWS
   * API Reference</a></p>
   */
  class TargetObjectKeyFormat
  {
  public:
    AWS_S3_API TargetObjectKeyFormat();
    AWS_S3_API TargetObjectKeyFormat(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API TargetObjectKeyFormat& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>To use the simple format for S3 keys for log objects. To specify SimplePrefix
     * format, set SimplePrefix to {}.</p>
     */
    inline const SimplePrefix& GetSimplePrefix() const{ return m_simplePrefix; }
    inline bool SimplePrefixHasBeenSet() const { return m_simplePrefixHasBeenSet; }
    inline void SetSimplePrefix(const SimplePrefix& value) { m_simplePrefixHasBeenSet = true; m_simplePrefix = value; }
    inline void SetSimplePrefix(SimplePrefix&& value) { m_simplePrefixHasBeenSet = true; m_simplePrefix = std::move(value); }
    inline TargetObjectKeyFormat& WithSimplePrefix(const SimplePrefix& value) { SetSimplePrefix(value); return *this;}
    inline TargetObjectKeyFormat& WithSimplePrefix(SimplePrefix&& value) { SetSimplePrefix(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Partitioned S3 key for log objects.</p>
     */
    inline const PartitionedPrefix& GetPartitionedPrefix() const{ return m_partitionedPrefix; }
    inline bool PartitionedPrefixHasBeenSet() const { return m_partitionedPrefixHasBeenSet; }
    inline void SetPartitionedPrefix(const PartitionedPrefix& value) { m_partitionedPrefixHasBeenSet = true; m_partitionedPrefix = value; }
    inline void SetPartitionedPrefix(PartitionedPrefix&& value) { m_partitionedPrefixHasBeenSet = true; m_partitionedPrefix = std::move(value); }
    inline TargetObjectKeyFormat& WithPartitionedPrefix(const PartitionedPrefix& value) { SetPartitionedPrefix(value); return *this;}
    inline TargetObjectKeyFormat& WithPartitionedPrefix(PartitionedPrefix&& value) { SetPartitionedPrefix(std::move(value)); return *this;}
    ///@}
  private:

    SimplePrefix m_simplePrefix;
    bool m_simplePrefixHasBeenSet = false;

    PartitionedPrefix m_partitionedPrefix;
    bool m_partitionedPrefixHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
