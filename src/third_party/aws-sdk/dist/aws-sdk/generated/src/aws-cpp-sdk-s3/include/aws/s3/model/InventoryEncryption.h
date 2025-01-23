/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/SSES3.h>
#include <aws/s3/model/SSEKMS.h>
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
   * <p>Contains the type of server-side encryption used to encrypt the inventory
   * results.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/InventoryEncryption">AWS
   * API Reference</a></p>
   */
  class InventoryEncryption
  {
  public:
    AWS_S3_API InventoryEncryption();
    AWS_S3_API InventoryEncryption(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API InventoryEncryption& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies the use of SSE-S3 to encrypt delivered inventory reports.</p>
     */
    inline const SSES3& GetSSES3() const{ return m_sSES3; }
    inline bool SSES3HasBeenSet() const { return m_sSES3HasBeenSet; }
    inline void SetSSES3(const SSES3& value) { m_sSES3HasBeenSet = true; m_sSES3 = value; }
    inline void SetSSES3(SSES3&& value) { m_sSES3HasBeenSet = true; m_sSES3 = std::move(value); }
    inline InventoryEncryption& WithSSES3(const SSES3& value) { SetSSES3(value); return *this;}
    inline InventoryEncryption& WithSSES3(SSES3&& value) { SetSSES3(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the use of SSE-KMS to encrypt delivered inventory reports.</p>
     */
    inline const SSEKMS& GetSSEKMS() const{ return m_sSEKMS; }
    inline bool SSEKMSHasBeenSet() const { return m_sSEKMSHasBeenSet; }
    inline void SetSSEKMS(const SSEKMS& value) { m_sSEKMSHasBeenSet = true; m_sSEKMS = value; }
    inline void SetSSEKMS(SSEKMS&& value) { m_sSEKMSHasBeenSet = true; m_sSEKMS = std::move(value); }
    inline InventoryEncryption& WithSSEKMS(const SSEKMS& value) { SetSSEKMS(value); return *this;}
    inline InventoryEncryption& WithSSEKMS(SSEKMS&& value) { SetSSEKMS(std::move(value)); return *this;}
    ///@}
  private:

    SSES3 m_sSES3;
    bool m_sSES3HasBeenSet = false;

    SSEKMS m_sSEKMS;
    bool m_sSEKMSHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
