/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/Payer.h>
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
   * <p>Container for Payer.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/RequestPaymentConfiguration">AWS
   * API Reference</a></p>
   */
  class RequestPaymentConfiguration
  {
  public:
    AWS_S3_API RequestPaymentConfiguration();
    AWS_S3_API RequestPaymentConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API RequestPaymentConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies who pays for the download and request fees.</p>
     */
    inline const Payer& GetPayer() const{ return m_payer; }
    inline bool PayerHasBeenSet() const { return m_payerHasBeenSet; }
    inline void SetPayer(const Payer& value) { m_payerHasBeenSet = true; m_payer = value; }
    inline void SetPayer(Payer&& value) { m_payerHasBeenSet = true; m_payer = std::move(value); }
    inline RequestPaymentConfiguration& WithPayer(const Payer& value) { SetPayer(value); return *this;}
    inline RequestPaymentConfiguration& WithPayer(Payer&& value) { SetPayer(std::move(value)); return *this;}
    ///@}
  private:

    Payer m_payer;
    bool m_payerHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
