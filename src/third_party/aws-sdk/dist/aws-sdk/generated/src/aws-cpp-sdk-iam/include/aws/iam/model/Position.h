/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace IAM
{
namespace Model
{

  /**
   * <p>Contains the row and column of a location of a <code>Statement</code> element
   * in a policy document.</p> <p>This data type is used as a member of the <code>
   * <a>Statement</a> </code> type.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/Position">AWS API
   * Reference</a></p>
   */
  class Position
  {
  public:
    AWS_IAM_API Position();
    AWS_IAM_API Position(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API Position& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The line containing the specified position in the document.</p>
     */
    inline int GetLine() const{ return m_line; }
    inline bool LineHasBeenSet() const { return m_lineHasBeenSet; }
    inline void SetLine(int value) { m_lineHasBeenSet = true; m_line = value; }
    inline Position& WithLine(int value) { SetLine(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The column in the line containing the specified position in the document.</p>
     */
    inline int GetColumn() const{ return m_column; }
    inline bool ColumnHasBeenSet() const { return m_columnHasBeenSet; }
    inline void SetColumn(int value) { m_columnHasBeenSet = true; m_column = value; }
    inline Position& WithColumn(int value) { SetColumn(value); return *this;}
    ///@}
  private:

    int m_line;
    bool m_lineHasBeenSet = false;

    int m_column;
    bool m_columnHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
