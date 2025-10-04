/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/S3Location.h>
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
   * <p>Describes the location where the restore job's output is
   * stored.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/OutputLocation">AWS
   * API Reference</a></p>
   */
  class OutputLocation
  {
  public:
    AWS_S3_API OutputLocation();
    AWS_S3_API OutputLocation(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API OutputLocation& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Describes an S3 location that will receive the results of the restore
     * request.</p>
     */
    inline const S3Location& GetS3() const{ return m_s3; }
    inline bool S3HasBeenSet() const { return m_s3HasBeenSet; }
    inline void SetS3(const S3Location& value) { m_s3HasBeenSet = true; m_s3 = value; }
    inline void SetS3(S3Location&& value) { m_s3HasBeenSet = true; m_s3 = std::move(value); }
    inline OutputLocation& WithS3(const S3Location& value) { SetS3(value); return *this;}
    inline OutputLocation& WithS3(S3Location&& value) { SetS3(std::move(value)); return *this;}
    ///@}
  private:

    S3Location m_s3;
    bool m_s3HasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
