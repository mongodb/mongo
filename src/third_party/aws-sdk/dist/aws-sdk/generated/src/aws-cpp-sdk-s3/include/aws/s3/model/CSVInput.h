/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/FileHeaderInfo.h>
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
   * <p>Describes how an uncompressed comma-separated values (CSV)-formatted input
   * object is formatted.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CSVInput">AWS API
   * Reference</a></p>
   */
  class CSVInput
  {
  public:
    AWS_S3_API CSVInput();
    AWS_S3_API CSVInput(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API CSVInput& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Describes the first line of input. Valid values are:</p> <ul> <li> <p>
     * <code>NONE</code>: First line is not a header.</p> </li> <li> <p>
     * <code>IGNORE</code>: First line is a header, but you can't use the header values
     * to indicate the column in an expression. You can use column position (such as
     * _1, _2, …) to indicate the column (<code>SELECT s._1 FROM OBJECT s</code>).</p>
     * </li> <li> <p> <code>Use</code>: First line is a header, and you can use the
     * header value to identify a column in an expression (<code>SELECT "name" FROM
     * OBJECT</code>). </p> </li> </ul>
     */
    inline const FileHeaderInfo& GetFileHeaderInfo() const{ return m_fileHeaderInfo; }
    inline bool FileHeaderInfoHasBeenSet() const { return m_fileHeaderInfoHasBeenSet; }
    inline void SetFileHeaderInfo(const FileHeaderInfo& value) { m_fileHeaderInfoHasBeenSet = true; m_fileHeaderInfo = value; }
    inline void SetFileHeaderInfo(FileHeaderInfo&& value) { m_fileHeaderInfoHasBeenSet = true; m_fileHeaderInfo = std::move(value); }
    inline CSVInput& WithFileHeaderInfo(const FileHeaderInfo& value) { SetFileHeaderInfo(value); return *this;}
    inline CSVInput& WithFileHeaderInfo(FileHeaderInfo&& value) { SetFileHeaderInfo(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A single character used to indicate that a row should be ignored when the
     * character is present at the start of that row. You can specify any character to
     * indicate a comment line. The default character is <code>#</code>.</p>
     * <p>Default: <code>#</code> </p>
     */
    inline const Aws::String& GetComments() const{ return m_comments; }
    inline bool CommentsHasBeenSet() const { return m_commentsHasBeenSet; }
    inline void SetComments(const Aws::String& value) { m_commentsHasBeenSet = true; m_comments = value; }
    inline void SetComments(Aws::String&& value) { m_commentsHasBeenSet = true; m_comments = std::move(value); }
    inline void SetComments(const char* value) { m_commentsHasBeenSet = true; m_comments.assign(value); }
    inline CSVInput& WithComments(const Aws::String& value) { SetComments(value); return *this;}
    inline CSVInput& WithComments(Aws::String&& value) { SetComments(std::move(value)); return *this;}
    inline CSVInput& WithComments(const char* value) { SetComments(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A single character used for escaping the quotation mark character inside an
     * already escaped value. For example, the value <code>""" a , b """</code> is
     * parsed as <code>" a , b "</code>.</p>
     */
    inline const Aws::String& GetQuoteEscapeCharacter() const{ return m_quoteEscapeCharacter; }
    inline bool QuoteEscapeCharacterHasBeenSet() const { return m_quoteEscapeCharacterHasBeenSet; }
    inline void SetQuoteEscapeCharacter(const Aws::String& value) { m_quoteEscapeCharacterHasBeenSet = true; m_quoteEscapeCharacter = value; }
    inline void SetQuoteEscapeCharacter(Aws::String&& value) { m_quoteEscapeCharacterHasBeenSet = true; m_quoteEscapeCharacter = std::move(value); }
    inline void SetQuoteEscapeCharacter(const char* value) { m_quoteEscapeCharacterHasBeenSet = true; m_quoteEscapeCharacter.assign(value); }
    inline CSVInput& WithQuoteEscapeCharacter(const Aws::String& value) { SetQuoteEscapeCharacter(value); return *this;}
    inline CSVInput& WithQuoteEscapeCharacter(Aws::String&& value) { SetQuoteEscapeCharacter(std::move(value)); return *this;}
    inline CSVInput& WithQuoteEscapeCharacter(const char* value) { SetQuoteEscapeCharacter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A single character used to separate individual records in the input. Instead
     * of the default value, you can specify an arbitrary delimiter.</p>
     */
    inline const Aws::String& GetRecordDelimiter() const{ return m_recordDelimiter; }
    inline bool RecordDelimiterHasBeenSet() const { return m_recordDelimiterHasBeenSet; }
    inline void SetRecordDelimiter(const Aws::String& value) { m_recordDelimiterHasBeenSet = true; m_recordDelimiter = value; }
    inline void SetRecordDelimiter(Aws::String&& value) { m_recordDelimiterHasBeenSet = true; m_recordDelimiter = std::move(value); }
    inline void SetRecordDelimiter(const char* value) { m_recordDelimiterHasBeenSet = true; m_recordDelimiter.assign(value); }
    inline CSVInput& WithRecordDelimiter(const Aws::String& value) { SetRecordDelimiter(value); return *this;}
    inline CSVInput& WithRecordDelimiter(Aws::String&& value) { SetRecordDelimiter(std::move(value)); return *this;}
    inline CSVInput& WithRecordDelimiter(const char* value) { SetRecordDelimiter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A single character used to separate individual fields in a record. You can
     * specify an arbitrary delimiter.</p>
     */
    inline const Aws::String& GetFieldDelimiter() const{ return m_fieldDelimiter; }
    inline bool FieldDelimiterHasBeenSet() const { return m_fieldDelimiterHasBeenSet; }
    inline void SetFieldDelimiter(const Aws::String& value) { m_fieldDelimiterHasBeenSet = true; m_fieldDelimiter = value; }
    inline void SetFieldDelimiter(Aws::String&& value) { m_fieldDelimiterHasBeenSet = true; m_fieldDelimiter = std::move(value); }
    inline void SetFieldDelimiter(const char* value) { m_fieldDelimiterHasBeenSet = true; m_fieldDelimiter.assign(value); }
    inline CSVInput& WithFieldDelimiter(const Aws::String& value) { SetFieldDelimiter(value); return *this;}
    inline CSVInput& WithFieldDelimiter(Aws::String&& value) { SetFieldDelimiter(std::move(value)); return *this;}
    inline CSVInput& WithFieldDelimiter(const char* value) { SetFieldDelimiter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A single character used for escaping when the field delimiter is part of the
     * value. For example, if the value is <code>a, b</code>, Amazon S3 wraps this
     * field value in quotation marks, as follows: <code>" a , b "</code>.</p> <p>Type:
     * String</p> <p>Default: <code>"</code> </p> <p>Ancestors: <code>CSV</code> </p>
     */
    inline const Aws::String& GetQuoteCharacter() const{ return m_quoteCharacter; }
    inline bool QuoteCharacterHasBeenSet() const { return m_quoteCharacterHasBeenSet; }
    inline void SetQuoteCharacter(const Aws::String& value) { m_quoteCharacterHasBeenSet = true; m_quoteCharacter = value; }
    inline void SetQuoteCharacter(Aws::String&& value) { m_quoteCharacterHasBeenSet = true; m_quoteCharacter = std::move(value); }
    inline void SetQuoteCharacter(const char* value) { m_quoteCharacterHasBeenSet = true; m_quoteCharacter.assign(value); }
    inline CSVInput& WithQuoteCharacter(const Aws::String& value) { SetQuoteCharacter(value); return *this;}
    inline CSVInput& WithQuoteCharacter(Aws::String&& value) { SetQuoteCharacter(std::move(value)); return *this;}
    inline CSVInput& WithQuoteCharacter(const char* value) { SetQuoteCharacter(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies that CSV field values may contain quoted record delimiters and such
     * records should be allowed. Default value is FALSE. Setting this value to TRUE
     * may lower performance.</p>
     */
    inline bool GetAllowQuotedRecordDelimiter() const{ return m_allowQuotedRecordDelimiter; }
    inline bool AllowQuotedRecordDelimiterHasBeenSet() const { return m_allowQuotedRecordDelimiterHasBeenSet; }
    inline void SetAllowQuotedRecordDelimiter(bool value) { m_allowQuotedRecordDelimiterHasBeenSet = true; m_allowQuotedRecordDelimiter = value; }
    inline CSVInput& WithAllowQuotedRecordDelimiter(bool value) { SetAllowQuotedRecordDelimiter(value); return *this;}
    ///@}
  private:

    FileHeaderInfo m_fileHeaderInfo;
    bool m_fileHeaderInfoHasBeenSet = false;

    Aws::String m_comments;
    bool m_commentsHasBeenSet = false;

    Aws::String m_quoteEscapeCharacter;
    bool m_quoteEscapeCharacterHasBeenSet = false;

    Aws::String m_recordDelimiter;
    bool m_recordDelimiterHasBeenSet = false;

    Aws::String m_fieldDelimiter;
    bool m_fieldDelimiterHasBeenSet = false;

    Aws::String m_quoteCharacter;
    bool m_quoteCharacterHasBeenSet = false;

    bool m_allowQuotedRecordDelimiter;
    bool m_allowQuotedRecordDelimiterHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
