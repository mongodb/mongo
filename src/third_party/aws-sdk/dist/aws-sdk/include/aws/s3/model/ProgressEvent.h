/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/Progress.h>
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
   * <p>This data type contains information about the progress event of an
   * operation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ProgressEvent">AWS
   * API Reference</a></p>
   */
  class ProgressEvent
  {
  public:
    AWS_S3_API ProgressEvent();
    AWS_S3_API ProgressEvent(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ProgressEvent& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The Progress event details.</p>
     */
    inline const Progress& GetDetails() const{ return m_details; }
    inline bool DetailsHasBeenSet() const { return m_detailsHasBeenSet; }
    inline void SetDetails(const Progress& value) { m_detailsHasBeenSet = true; m_details = value; }
    inline void SetDetails(Progress&& value) { m_detailsHasBeenSet = true; m_details = std::move(value); }
    inline ProgressEvent& WithDetails(const Progress& value) { SetDetails(value); return *this;}
    inline ProgressEvent& WithDetails(Progress&& value) { SetDetails(std::move(value)); return *this;}
    ///@}
  private:

    Progress m_details;
    bool m_detailsHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
