/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/InputSerialization.h>
#include <aws/s3/model/ExpressionType.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/OutputSerialization.h>
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
   *  <p>Amazon S3 Select is no longer available to new customers.
   * Existing customers of Amazon S3 Select can continue to use the feature as usual.
   * <a
   * href="http://aws.amazon.com/blogs/storage/how-to-optimize-querying-your-data-in-amazon-s3/">Learn
   * more</a> </p>  <p>Describes the parameters for Select job types.</p>
   * <p>Learn <a
   * href="http://aws.amazon.com/blogs/storage/how-to-optimize-querying-your-data-in-amazon-s3/">How
   * to optimize querying your data in Amazon S3</a> using <a
   * href="https://docs.aws.amazon.com/athena/latest/ug/what-is.html">Amazon
   * Athena</a>, <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/transforming-objects.html">S3
   * Object Lambda</a>, or client-side filtering.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/SelectParameters">AWS
   * API Reference</a></p>
   */
  class SelectParameters
  {
  public:
    AWS_S3_API SelectParameters();
    AWS_S3_API SelectParameters(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API SelectParameters& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Describes the serialization format of the object.</p>
     */
    inline const InputSerialization& GetInputSerialization() const{ return m_inputSerialization; }
    inline bool InputSerializationHasBeenSet() const { return m_inputSerializationHasBeenSet; }
    inline void SetInputSerialization(const InputSerialization& value) { m_inputSerializationHasBeenSet = true; m_inputSerialization = value; }
    inline void SetInputSerialization(InputSerialization&& value) { m_inputSerializationHasBeenSet = true; m_inputSerialization = std::move(value); }
    inline SelectParameters& WithInputSerialization(const InputSerialization& value) { SetInputSerialization(value); return *this;}
    inline SelectParameters& WithInputSerialization(InputSerialization&& value) { SetInputSerialization(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The type of the provided expression (for example, SQL).</p>
     */
    inline const ExpressionType& GetExpressionType() const{ return m_expressionType; }
    inline bool ExpressionTypeHasBeenSet() const { return m_expressionTypeHasBeenSet; }
    inline void SetExpressionType(const ExpressionType& value) { m_expressionTypeHasBeenSet = true; m_expressionType = value; }
    inline void SetExpressionType(ExpressionType&& value) { m_expressionTypeHasBeenSet = true; m_expressionType = std::move(value); }
    inline SelectParameters& WithExpressionType(const ExpressionType& value) { SetExpressionType(value); return *this;}
    inline SelectParameters& WithExpressionType(ExpressionType&& value) { SetExpressionType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     *  <p>Amazon S3 Select is no longer available to new customers.
     * Existing customers of Amazon S3 Select can continue to use the feature as usual.
     * <a
     * href="http://aws.amazon.com/blogs/storage/how-to-optimize-querying-your-data-in-amazon-s3/">Learn
     * more</a> </p>  <p>The expression that is used to query the
     * object.</p>
     */
    inline const Aws::String& GetExpression() const{ return m_expression; }
    inline bool ExpressionHasBeenSet() const { return m_expressionHasBeenSet; }
    inline void SetExpression(const Aws::String& value) { m_expressionHasBeenSet = true; m_expression = value; }
    inline void SetExpression(Aws::String&& value) { m_expressionHasBeenSet = true; m_expression = std::move(value); }
    inline void SetExpression(const char* value) { m_expressionHasBeenSet = true; m_expression.assign(value); }
    inline SelectParameters& WithExpression(const Aws::String& value) { SetExpression(value); return *this;}
    inline SelectParameters& WithExpression(Aws::String&& value) { SetExpression(std::move(value)); return *this;}
    inline SelectParameters& WithExpression(const char* value) { SetExpression(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Describes how the results of the Select job are serialized.</p>
     */
    inline const OutputSerialization& GetOutputSerialization() const{ return m_outputSerialization; }
    inline bool OutputSerializationHasBeenSet() const { return m_outputSerializationHasBeenSet; }
    inline void SetOutputSerialization(const OutputSerialization& value) { m_outputSerializationHasBeenSet = true; m_outputSerialization = value; }
    inline void SetOutputSerialization(OutputSerialization&& value) { m_outputSerializationHasBeenSet = true; m_outputSerialization = std::move(value); }
    inline SelectParameters& WithOutputSerialization(const OutputSerialization& value) { SetOutputSerialization(value); return *this;}
    inline SelectParameters& WithOutputSerialization(OutputSerialization&& value) { SetOutputSerialization(std::move(value)); return *this;}
    ///@}
  private:

    InputSerialization m_inputSerialization;
    bool m_inputSerializationHasBeenSet = false;

    ExpressionType m_expressionType;
    bool m_expressionTypeHasBeenSet = false;

    Aws::String m_expression;
    bool m_expressionHasBeenSet = false;

    OutputSerialization m_outputSerialization;
    bool m_outputSerializationHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
