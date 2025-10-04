/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/CSVOutput.h>
#include <aws/s3/model/JSONOutput.h>
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
   * <p>Describes how results of the Select job are serialized.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/OutputSerialization">AWS
   * API Reference</a></p>
   */
  class OutputSerialization
  {
  public:
    AWS_S3_API OutputSerialization();
    AWS_S3_API OutputSerialization(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API OutputSerialization& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Describes the serialization of CSV-encoded Select results.</p>
     */
    inline const CSVOutput& GetCSV() const{ return m_cSV; }
    inline bool CSVHasBeenSet() const { return m_cSVHasBeenSet; }
    inline void SetCSV(const CSVOutput& value) { m_cSVHasBeenSet = true; m_cSV = value; }
    inline void SetCSV(CSVOutput&& value) { m_cSVHasBeenSet = true; m_cSV = std::move(value); }
    inline OutputSerialization& WithCSV(const CSVOutput& value) { SetCSV(value); return *this;}
    inline OutputSerialization& WithCSV(CSVOutput&& value) { SetCSV(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies JSON as request's output serialization format.</p>
     */
    inline const JSONOutput& GetJSON() const{ return m_jSON; }
    inline bool JSONHasBeenSet() const { return m_jSONHasBeenSet; }
    inline void SetJSON(const JSONOutput& value) { m_jSONHasBeenSet = true; m_jSON = value; }
    inline void SetJSON(JSONOutput&& value) { m_jSONHasBeenSet = true; m_jSON = std::move(value); }
    inline OutputSerialization& WithJSON(const JSONOutput& value) { SetJSON(value); return *this;}
    inline OutputSerialization& WithJSON(JSONOutput&& value) { SetJSON(std::move(value)); return *this;}
    ///@}
  private:

    CSVOutput m_cSV;
    bool m_cSVHasBeenSet = false;

    JSONOutput m_jSON;
    bool m_jSONHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
