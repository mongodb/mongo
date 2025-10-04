/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ExistingObjectReplicationStatus.h>
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
   * <p>Optional configuration to replicate existing source bucket objects. </p>
   *  <p>This parameter is no longer supported. To replicate existing objects,
   * see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-batch-replication-batch.html">Replicating
   * existing objects with S3 Batch Replication</a> in the <i>Amazon S3 User
   * Guide</i>.</p> <p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ExistingObjectReplication">AWS
   * API Reference</a></p>
   */
  class ExistingObjectReplication
  {
  public:
    AWS_S3_API ExistingObjectReplication();
    AWS_S3_API ExistingObjectReplication(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ExistingObjectReplication& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies whether Amazon S3 replicates existing source bucket objects. </p>
     */
    inline const ExistingObjectReplicationStatus& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const ExistingObjectReplicationStatus& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(ExistingObjectReplicationStatus&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline ExistingObjectReplication& WithStatus(const ExistingObjectReplicationStatus& value) { SetStatus(value); return *this;}
    inline ExistingObjectReplication& WithStatus(ExistingObjectReplicationStatus&& value) { SetStatus(std::move(value)); return *this;}
    ///@}
  private:

    ExistingObjectReplicationStatus m_status;
    bool m_statusHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
