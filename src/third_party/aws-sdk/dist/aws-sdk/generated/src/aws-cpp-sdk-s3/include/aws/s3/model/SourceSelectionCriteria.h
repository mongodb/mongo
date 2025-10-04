/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/SseKmsEncryptedObjects.h>
#include <aws/s3/model/ReplicaModifications.h>
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
   * <p>A container that describes additional filters for identifying the source
   * objects that you want to replicate. You can choose to enable or disable the
   * replication of these objects. Currently, Amazon S3 supports only the filter that
   * you can specify for objects created with server-side encryption using a customer
   * managed key stored in Amazon Web Services Key Management Service
   * (SSE-KMS).</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/SourceSelectionCriteria">AWS
   * API Reference</a></p>
   */
  class SourceSelectionCriteria
  {
  public:
    AWS_S3_API SourceSelectionCriteria();
    AWS_S3_API SourceSelectionCriteria(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API SourceSelectionCriteria& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p> A container for filter information for the selection of Amazon S3 objects
     * encrypted with Amazon Web Services KMS. If you include
     * <code>SourceSelectionCriteria</code> in the replication configuration, this
     * element is required. </p>
     */
    inline const SseKmsEncryptedObjects& GetSseKmsEncryptedObjects() const{ return m_sseKmsEncryptedObjects; }
    inline bool SseKmsEncryptedObjectsHasBeenSet() const { return m_sseKmsEncryptedObjectsHasBeenSet; }
    inline void SetSseKmsEncryptedObjects(const SseKmsEncryptedObjects& value) { m_sseKmsEncryptedObjectsHasBeenSet = true; m_sseKmsEncryptedObjects = value; }
    inline void SetSseKmsEncryptedObjects(SseKmsEncryptedObjects&& value) { m_sseKmsEncryptedObjectsHasBeenSet = true; m_sseKmsEncryptedObjects = std::move(value); }
    inline SourceSelectionCriteria& WithSseKmsEncryptedObjects(const SseKmsEncryptedObjects& value) { SetSseKmsEncryptedObjects(value); return *this;}
    inline SourceSelectionCriteria& WithSseKmsEncryptedObjects(SseKmsEncryptedObjects&& value) { SetSseKmsEncryptedObjects(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A filter that you can specify for selections for modifications on replicas.
     * Amazon S3 doesn't replicate replica modifications by default. In the latest
     * version of replication configuration (when <code>Filter</code> is specified),
     * you can specify this element and set the status to <code>Enabled</code> to
     * replicate modifications on replicas. </p>  <p> If you don't specify the
     * <code>Filter</code> element, Amazon S3 assumes that the replication
     * configuration is the earlier version, V1. In the earlier version, this element
     * is not allowed</p> 
     */
    inline const ReplicaModifications& GetReplicaModifications() const{ return m_replicaModifications; }
    inline bool ReplicaModificationsHasBeenSet() const { return m_replicaModificationsHasBeenSet; }
    inline void SetReplicaModifications(const ReplicaModifications& value) { m_replicaModificationsHasBeenSet = true; m_replicaModifications = value; }
    inline void SetReplicaModifications(ReplicaModifications&& value) { m_replicaModificationsHasBeenSet = true; m_replicaModifications = std::move(value); }
    inline SourceSelectionCriteria& WithReplicaModifications(const ReplicaModifications& value) { SetReplicaModifications(value); return *this;}
    inline SourceSelectionCriteria& WithReplicaModifications(ReplicaModifications&& value) { SetReplicaModifications(std::move(value)); return *this;}
    ///@}
  private:

    SseKmsEncryptedObjects m_sseKmsEncryptedObjects;
    bool m_sseKmsEncryptedObjectsHasBeenSet = false;

    ReplicaModifications m_replicaModifications;
    bool m_replicaModificationsHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
