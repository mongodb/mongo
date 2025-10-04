/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{

  /**
   * <p> A structure within a <code>FilterCriteria</code> object that defines an
   * event filtering pattern. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/Filter">AWS API
   * Reference</a></p>
   */
  class Filter
  {
  public:
    AWS_LAMBDA_API Filter();
    AWS_LAMBDA_API Filter(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Filter& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p> A filter pattern. For more information on the syntax of a filter pattern,
     * see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-eventfiltering.html#filtering-syntax">
     * Filter rule syntax</a>. </p>
     */
    inline const Aws::String& GetPattern() const{ return m_pattern; }
    inline bool PatternHasBeenSet() const { return m_patternHasBeenSet; }
    inline void SetPattern(const Aws::String& value) { m_patternHasBeenSet = true; m_pattern = value; }
    inline void SetPattern(Aws::String&& value) { m_patternHasBeenSet = true; m_pattern = std::move(value); }
    inline void SetPattern(const char* value) { m_patternHasBeenSet = true; m_pattern.assign(value); }
    inline Filter& WithPattern(const Aws::String& value) { SetPattern(value); return *this;}
    inline Filter& WithPattern(Aws::String&& value) { SetPattern(std::move(value)); return *this;}
    inline Filter& WithPattern(const char* value) { SetPattern(value); return *this;}
    ///@}
  private:

    Aws::String m_pattern;
    bool m_patternHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
