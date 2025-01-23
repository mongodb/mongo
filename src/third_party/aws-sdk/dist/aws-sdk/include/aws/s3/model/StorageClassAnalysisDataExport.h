/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/StorageClassAnalysisSchemaVersion.h>
#include <aws/s3/model/AnalyticsExportDestination.h>
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
   * <p>Container for data related to the storage class analysis for an Amazon S3
   * bucket for export.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/StorageClassAnalysisDataExport">AWS
   * API Reference</a></p>
   */
  class StorageClassAnalysisDataExport
  {
  public:
    AWS_S3_API StorageClassAnalysisDataExport();
    AWS_S3_API StorageClassAnalysisDataExport(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API StorageClassAnalysisDataExport& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The version of the output schema to use when exporting data. Must be
     * <code>V_1</code>.</p>
     */
    inline const StorageClassAnalysisSchemaVersion& GetOutputSchemaVersion() const{ return m_outputSchemaVersion; }
    inline bool OutputSchemaVersionHasBeenSet() const { return m_outputSchemaVersionHasBeenSet; }
    inline void SetOutputSchemaVersion(const StorageClassAnalysisSchemaVersion& value) { m_outputSchemaVersionHasBeenSet = true; m_outputSchemaVersion = value; }
    inline void SetOutputSchemaVersion(StorageClassAnalysisSchemaVersion&& value) { m_outputSchemaVersionHasBeenSet = true; m_outputSchemaVersion = std::move(value); }
    inline StorageClassAnalysisDataExport& WithOutputSchemaVersion(const StorageClassAnalysisSchemaVersion& value) { SetOutputSchemaVersion(value); return *this;}
    inline StorageClassAnalysisDataExport& WithOutputSchemaVersion(StorageClassAnalysisSchemaVersion&& value) { SetOutputSchemaVersion(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The place to store the data for an analysis.</p>
     */
    inline const AnalyticsExportDestination& GetDestination() const{ return m_destination; }
    inline bool DestinationHasBeenSet() const { return m_destinationHasBeenSet; }
    inline void SetDestination(const AnalyticsExportDestination& value) { m_destinationHasBeenSet = true; m_destination = value; }
    inline void SetDestination(AnalyticsExportDestination&& value) { m_destinationHasBeenSet = true; m_destination = std::move(value); }
    inline StorageClassAnalysisDataExport& WithDestination(const AnalyticsExportDestination& value) { SetDestination(value); return *this;}
    inline StorageClassAnalysisDataExport& WithDestination(AnalyticsExportDestination&& value) { SetDestination(std::move(value)); return *this;}
    ///@}
  private:

    StorageClassAnalysisSchemaVersion m_outputSchemaVersion;
    bool m_outputSchemaVersionHasBeenSet = false;

    AnalyticsExportDestination m_destination;
    bool m_destinationHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
