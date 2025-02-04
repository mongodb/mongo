/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/QuoteFields.h>
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
   * <p>Describes how uncompressed comma-separated values (CSV)-formatted results are
   * formatted.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CSVOutput">AWS API
   * Reference</a></p>
   */
  class CSVOutput
  {
  public:
    AWS_S3_API CSVOutput();
    AWS_S3_API CSVOutput(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API CSVOutput& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Indicates whether to use quotation marks around output fields. </p> <ul> <li>
     * <p> <code>ALWAYS</code>: Always use quotation marks for output fields.</p> </li>
     * <li> <p> <code>ASNEEDED</code>: Use quotation marks for output fields when
     * needed.</p> </li> </ul>
     */
    inline const QuoteFields& GetQuoteFields() const{ return m_quoteFields; }
    inline bool QuoteFieldsHasBeenSet() const { return m_quoteFieldsHasBeenSet; }
    inline void SetQuoteFields(const QuoteFields& value) { m_quoteFieldsHasBeenSet = true; m_quoteFields = value; }
    inline void SetQuoteFields(QuoteFields&& value) { m_quoteFieldsHasBeenSet = true; m_quoteFields = std::move(value); }
    inline CSVOutput& WithQuoteFields(const QuoteFields& value) { SetQuoteFields(value); return *this;}
    inline CSVOutput& WithQuoteFields(QuoteFields&& value) { SetQuoteFields(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The single character used for escaping the quote character inside an already
     * escaped value.</p>
     */
    inline const Aws::String& GetQuoteEscapeCharacter() const{ return m_quoteEscapeCharacter; }
    inline bool QuoteEscapeCharacterHasBeenSet() const { return m_quoteEscapeCharacterHasBeenSet; }
    inline void SetQuoteEscapeCharacter(const Aws::String& value) { m_quoteEscapeCharacterHasBeenSet = true; m_quoteEscapeCharacter = value; }
    inline void SetQuoteEscapeCharacter(Aws::String&& value) { m_quoteEscapeCharacterHasBeenSet = true; m_quoteEscapeCharacter = std::move(value); }
    inline void SetQuoteEscapeCharacter(const char* value) { m_quoteEscapeCharacterHasBeenSet = true; m_quoteEscapeCharacter.assign(value); }
    inline CSVOutput& WithQuoteEscapeCharacter(const Aws::String& value) { SetQuoteEscapeCharacter(value); return *this;}
    inline CSVOutput& WithQuoteEscapeCharacter(Aws::String&& value) { SetQuoteEscapeCharacter(std::move(value)); return *this;}
    inline CSVOutput& WithQuoteEscapeCharacter(const char* value) { SetQuoteEscapeCharacter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A single character used to separate individual records in the output. Instead
     * of the default value, you can specify an arbitrary delimiter.</p>
     */
    inline const Aws::String& GetRecordDelimiter() const{ return m_recordDelimiter; }
    inline bool RecordDelimiterHasBeenSet() const { return m_recordDelimiterHasBeenSet; }
    inline void SetRecordDelimiter(const Aws::String& value) { m_recordDelimiterHasBeenSet = true; m_recordDelimiter = value; }
    inline void SetRecordDelimiter(Aws::String&& value) { m_recordDelimiterHasBeenSet = true; m_recordDelimiter = std::move(value); }
    inline void SetRecordDelimiter(const char* value) { m_recordDelimiterHasBeenSet = true; m_recordDelimiter.assign(value); }
    inline CSVOutput& WithRecordDelimiter(const Aws::String& value) { SetRecordDelimiter(value); return *this;}
    inline CSVOutput& WithRecordDelimiter(Aws::String&& value) { SetRecordDelimiter(std::move(value)); return *this;}
    inline CSVOutput& WithRecordDelimiter(const char* value) { SetRecordDelimiter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The value used to separate individual fields in a record. You can specify an
     * arbitrary delimiter.</p>
     */
    inline const Aws::String& GetFieldDelimiter() const{ return m_fieldDelimiter; }
    inline bool FieldDelimiterHasBeenSet() const { return m_fieldDelimiterHasBeenSet; }
    inline void SetFieldDelimiter(const Aws::String& value) { m_fieldDelimiterHasBeenSet = true; m_fieldDelimiter = value; }
    inline void SetFieldDelimiter(Aws::String&& value) { m_fieldDelimiterHasBeenSet = true; m_fieldDelimiter = std::move(value); }
    inline void SetFieldDelimiter(const char* value) { m_fieldDelimiterHasBeenSet = true; m_fieldDelimiter.assign(value); }
    inline CSVOutput& WithFieldDelimiter(const Aws::String& value) { SetFieldDelimiter(value); return *this;}
    inline CSVOutput& WithFieldDelimiter(Aws::String&& value) { SetFieldDelimiter(std::move(value)); return *this;}
    inline CSVOutput& WithFieldDelimiter(const char* value) { SetFieldDelimiter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A single character used for escaping when the field delimiter is part of the
     * value. For example, if the value is <code>a, b</code>, Amazon S3 wraps this
     * field value in quotation marks, as follows: <code>" a , b "</code>.</p>
     */
    inline const Aws::String& GetQuoteCharacter() const{ return m_quoteCharacter; }
    inline bool QuoteCharacterHasBeenSet() const { return m_quoteCharacterHasBeenSet; }
    inline void SetQuoteCharacter(const Aws::String& value) { m_quoteCharacterHasBeenSet = true; m_quoteCharacter = value; }
    inline void SetQuoteCharacter(Aws::String&& value) { m_quoteCharacterHasBeenSet = true; m_quoteCharacter = std::move(value); }
    inline void SetQuoteCharacter(const char* value) { m_quoteCharacterHasBeenSet = true; m_quoteCharacter.assign(value); }
    inline CSVOutput& WithQuoteCharacter(const Aws::String& value) { SetQuoteCharacter(value); return *this;}
    inline CSVOutput& WithQuoteCharacter(Aws::String&& value) { SetQuoteCharacter(std::move(value)); return *this;}
    inline CSVOutput& WithQuoteCharacter(const char* value) { SetQuoteCharacter(value); return *this;}
    ///@}
  private:

    QuoteFields m_quoteFields;
    bool m_quoteFieldsHasBeenSet = false;

    Aws::String m_quoteEscapeCharacter;
    bool m_quoteEscapeCharacterHasBeenSet = false;

    Aws::String m_recordDelimiter;
    bool m_recordDelimiterHasBeenSet = false;

    Aws::String m_fieldDelimiter;
    bool m_fieldDelimiterHasBeenSet = false;

    Aws::String m_quoteCharacter;
    bool m_quoteCharacterHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
