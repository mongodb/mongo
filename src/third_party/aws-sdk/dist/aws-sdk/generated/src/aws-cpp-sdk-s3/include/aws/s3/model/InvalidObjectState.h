/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/StorageClass.h>
#include <aws/s3/model/IntelligentTieringAccessTier.h>
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
   * <p>Object is archived and inaccessible until restored.</p> <p>If the object you
   * are retrieving is stored in the S3 Glacier Flexible Retrieval storage class, the
   * S3 Glacier Deep Archive storage class, the S3 Intelligent-Tiering Archive Access
   * tier, or the S3 Intelligent-Tiering Deep Archive Access tier, before you can
   * retrieve the object you must first restore a copy using <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_RestoreObject.html">RestoreObject</a>.
   * Otherwise, this operation returns an <code>InvalidObjectState</code> error. For
   * information about restoring archived objects, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/restoring-objects.html">Restoring
   * Archived Objects</a> in the <i>Amazon S3 User Guide</i>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/InvalidObjectState">AWS
   * API Reference</a></p>
   */
  class InvalidObjectState
  {
  public:
    AWS_S3_API InvalidObjectState();
    AWS_S3_API InvalidObjectState(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API InvalidObjectState& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    
    inline const StorageClass& GetStorageClass() const{ return m_storageClass; }
    inline bool StorageClassHasBeenSet() const { return m_storageClassHasBeenSet; }
    inline void SetStorageClass(const StorageClass& value) { m_storageClassHasBeenSet = true; m_storageClass = value; }
    inline void SetStorageClass(StorageClass&& value) { m_storageClassHasBeenSet = true; m_storageClass = std::move(value); }
    inline InvalidObjectState& WithStorageClass(const StorageClass& value) { SetStorageClass(value); return *this;}
    inline InvalidObjectState& WithStorageClass(StorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const IntelligentTieringAccessTier& GetAccessTier() const{ return m_accessTier; }
    inline bool AccessTierHasBeenSet() const { return m_accessTierHasBeenSet; }
    inline void SetAccessTier(const IntelligentTieringAccessTier& value) { m_accessTierHasBeenSet = true; m_accessTier = value; }
    inline void SetAccessTier(IntelligentTieringAccessTier&& value) { m_accessTierHasBeenSet = true; m_accessTier = std::move(value); }
    inline InvalidObjectState& WithAccessTier(const IntelligentTieringAccessTier& value) { SetAccessTier(value); return *this;}
    inline InvalidObjectState& WithAccessTier(IntelligentTieringAccessTier&& value) { SetAccessTier(std::move(value)); return *this;}
    ///@}
  private:

    StorageClass m_storageClass;
    bool m_storageClassHasBeenSet = false;

    IntelligentTieringAccessTier m_accessTier;
    bool m_accessTierHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
