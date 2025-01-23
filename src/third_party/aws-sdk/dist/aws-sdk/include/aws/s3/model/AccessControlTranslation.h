/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/OwnerOverride.h>
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
   * <p>A container for information about access control for replicas.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/AccessControlTranslation">AWS
   * API Reference</a></p>
   */
  class AccessControlTranslation
  {
  public:
    AWS_S3_API AccessControlTranslation();
    AWS_S3_API AccessControlTranslation(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API AccessControlTranslation& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies the replica ownership. For default and valid values, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTBucketPUTreplication.html">PUT
     * bucket replication</a> in the <i>Amazon S3 API Reference</i>.</p>
     */
    inline const OwnerOverride& GetOwner() const{ return m_owner; }
    inline bool OwnerHasBeenSet() const { return m_ownerHasBeenSet; }
    inline void SetOwner(const OwnerOverride& value) { m_ownerHasBeenSet = true; m_owner = value; }
    inline void SetOwner(OwnerOverride&& value) { m_ownerHasBeenSet = true; m_owner = std::move(value); }
    inline AccessControlTranslation& WithOwner(const OwnerOverride& value) { SetOwner(value); return *this;}
    inline AccessControlTranslation& WithOwner(OwnerOverride&& value) { SetOwner(std::move(value)); return *this;}
    ///@}
  private:

    OwnerOverride m_owner;
    bool m_ownerHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
