/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>Specifies JSON as request's output serialization format.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/JSONOutput">AWS API
   * Reference</a></p>
   */
  class JSONOutput
  {
  public:
    AWS_S3_API JSONOutput();
    AWS_S3_API JSONOutput(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API JSONOutput& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The value used to separate individual records in the output. If no value is
     * specified, Amazon S3 uses a newline character ('\n').</p>
     */
    inline const Aws::String& GetRecordDelimiter() const{ return m_recordDelimiter; }
    inline bool RecordDelimiterHasBeenSet() const { return m_recordDelimiterHasBeenSet; }
    inline void SetRecordDelimiter(const Aws::String& value) { m_recordDelimiterHasBeenSet = true; m_recordDelimiter = value; }
    inline void SetRecordDelimiter(Aws::String&& value) { m_recordDelimiterHasBeenSet = true; m_recordDelimiter = std::move(value); }
    inline void SetRecordDelimiter(const char* value) { m_recordDelimiterHasBeenSet = true; m_recordDelimiter.assign(value); }
    inline JSONOutput& WithRecordDelimiter(const Aws::String& value) { SetRecordDelimiter(value); return *this;}
    inline JSONOutput& WithRecordDelimiter(Aws::String&& value) { SetRecordDelimiter(std::move(value)); return *this;}
    inline JSONOutput& WithRecordDelimiter(const char* value) { SetRecordDelimiter(value); return *this;}
    ///@}
  private:

    Aws::String m_recordDelimiter;
    bool m_recordDelimiterHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
