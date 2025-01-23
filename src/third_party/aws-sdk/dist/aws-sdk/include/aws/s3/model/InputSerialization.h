/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/CSVInput.h>
#include <aws/s3/model/CompressionType.h>
#include <aws/s3/model/JSONInput.h>
#include <aws/s3/model/ParquetInput.h>
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
   * <p>Describes the serialization format of the object.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/InputSerialization">AWS
   * API Reference</a></p>
   */
  class InputSerialization
  {
  public:
    AWS_S3_API InputSerialization();
    AWS_S3_API InputSerialization(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API InputSerialization& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Describes the serialization of a CSV-encoded object.</p>
     */
    inline const CSVInput& GetCSV() const{ return m_cSV; }
    inline bool CSVHasBeenSet() const { return m_cSVHasBeenSet; }
    inline void SetCSV(const CSVInput& value) { m_cSVHasBeenSet = true; m_cSV = value; }
    inline void SetCSV(CSVInput&& value) { m_cSVHasBeenSet = true; m_cSV = std::move(value); }
    inline InputSerialization& WithCSV(const CSVInput& value) { SetCSV(value); return *this;}
    inline InputSerialization& WithCSV(CSVInput&& value) { SetCSV(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies object's compression format. Valid values: NONE, GZIP, BZIP2.
     * Default Value: NONE.</p>
     */
    inline const CompressionType& GetCompressionType() const{ return m_compressionType; }
    inline bool CompressionTypeHasBeenSet() const { return m_compressionTypeHasBeenSet; }
    inline void SetCompressionType(const CompressionType& value) { m_compressionTypeHasBeenSet = true; m_compressionType = value; }
    inline void SetCompressionType(CompressionType&& value) { m_compressionTypeHasBeenSet = true; m_compressionType = std::move(value); }
    inline InputSerialization& WithCompressionType(const CompressionType& value) { SetCompressionType(value); return *this;}
    inline InputSerialization& WithCompressionType(CompressionType&& value) { SetCompressionType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies JSON as object's input serialization format.</p>
     */
    inline const JSONInput& GetJSON() const{ return m_jSON; }
    inline bool JSONHasBeenSet() const { return m_jSONHasBeenSet; }
    inline void SetJSON(const JSONInput& value) { m_jSONHasBeenSet = true; m_jSON = value; }
    inline void SetJSON(JSONInput&& value) { m_jSONHasBeenSet = true; m_jSON = std::move(value); }
    inline InputSerialization& WithJSON(const JSONInput& value) { SetJSON(value); return *this;}
    inline InputSerialization& WithJSON(JSONInput&& value) { SetJSON(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies Parquet as object's input serialization format.</p>
     */
    inline const ParquetInput& GetParquet() const{ return m_parquet; }
    inline bool ParquetHasBeenSet() const { return m_parquetHasBeenSet; }
    inline void SetParquet(const ParquetInput& value) { m_parquetHasBeenSet = true; m_parquet = value; }
    inline void SetParquet(ParquetInput&& value) { m_parquetHasBeenSet = true; m_parquet = std::move(value); }
    inline InputSerialization& WithParquet(const ParquetInput& value) { SetParquet(value); return *this;}
    inline InputSerialization& WithParquet(ParquetInput&& value) { SetParquet(std::move(value)); return *this;}
    ///@}
  private:

    CSVInput m_cSV;
    bool m_cSVHasBeenSet = false;

    CompressionType m_compressionType;
    bool m_compressionTypeHasBeenSet = false;

    JSONInput m_jSON;
    bool m_jSONHasBeenSet = false;

    ParquetInput m_parquet;
    bool m_parquetHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
