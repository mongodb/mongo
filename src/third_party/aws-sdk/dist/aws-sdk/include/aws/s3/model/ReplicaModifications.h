/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ReplicaModificationsStatus.h>
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
   * <p>A filter that you can specify for selection for modifications on replicas.
   * Amazon S3 doesn't replicate replica modifications by default. In the latest
   * version of replication configuration (when <code>Filter</code> is specified),
   * you can specify this element and set the status to <code>Enabled</code> to
   * replicate modifications on replicas. </p>  <p> If you don't specify the
   * <code>Filter</code> element, Amazon S3 assumes that the replication
   * configuration is the earlier version, V1. In the earlier version, this element
   * is not allowed.</p> <p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ReplicaModifications">AWS
   * API Reference</a></p>
   */
  class ReplicaModifications
  {
  public:
    AWS_S3_API ReplicaModifications();
    AWS_S3_API ReplicaModifications(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ReplicaModifications& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies whether Amazon S3 replicates modifications on replicas.</p>
     */
    inline const ReplicaModificationsStatus& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const ReplicaModificationsStatus& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(ReplicaModificationsStatus&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline ReplicaModifications& WithStatus(const ReplicaModificationsStatus& value) { SetStatus(value); return *this;}
    inline ReplicaModifications& WithStatus(ReplicaModificationsStatus&& value) { SetStatus(std::move(value)); return *this;}
    ///@}
  private:

    ReplicaModificationsStatus m_status;
    bool m_statusHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
