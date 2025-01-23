/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/PolicySourceType.h>
#include <aws/iam/model/Position.h>
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
namespace IAM
{
namespace Model
{

  /**
   * <p>Contains a reference to a <code>Statement</code> element in a policy document
   * that determines the result of the simulation.</p> <p>This data type is used by
   * the <code>MatchedStatements</code> member of the <code> <a>EvaluationResult</a>
   * </code> type.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/Statement">AWS API
   * Reference</a></p>
   */
  class Statement
  {
  public:
    AWS_IAM_API Statement();
    AWS_IAM_API Statement(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API Statement& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The identifier of the policy that was provided as an input.</p>
     */
    inline const Aws::String& GetSourcePolicyId() const{ return m_sourcePolicyId; }
    inline bool SourcePolicyIdHasBeenSet() const { return m_sourcePolicyIdHasBeenSet; }
    inline void SetSourcePolicyId(const Aws::String& value) { m_sourcePolicyIdHasBeenSet = true; m_sourcePolicyId = value; }
    inline void SetSourcePolicyId(Aws::String&& value) { m_sourcePolicyIdHasBeenSet = true; m_sourcePolicyId = std::move(value); }
    inline void SetSourcePolicyId(const char* value) { m_sourcePolicyIdHasBeenSet = true; m_sourcePolicyId.assign(value); }
    inline Statement& WithSourcePolicyId(const Aws::String& value) { SetSourcePolicyId(value); return *this;}
    inline Statement& WithSourcePolicyId(Aws::String&& value) { SetSourcePolicyId(std::move(value)); return *this;}
    inline Statement& WithSourcePolicyId(const char* value) { SetSourcePolicyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The type of the policy.</p>
     */
    inline const PolicySourceType& GetSourcePolicyType() const{ return m_sourcePolicyType; }
    inline bool SourcePolicyTypeHasBeenSet() const { return m_sourcePolicyTypeHasBeenSet; }
    inline void SetSourcePolicyType(const PolicySourceType& value) { m_sourcePolicyTypeHasBeenSet = true; m_sourcePolicyType = value; }
    inline void SetSourcePolicyType(PolicySourceType&& value) { m_sourcePolicyTypeHasBeenSet = true; m_sourcePolicyType = std::move(value); }
    inline Statement& WithSourcePolicyType(const PolicySourceType& value) { SetSourcePolicyType(value); return *this;}
    inline Statement& WithSourcePolicyType(PolicySourceType&& value) { SetSourcePolicyType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The row and column of the beginning of the <code>Statement</code> in an IAM
     * policy.</p>
     */
    inline const Position& GetStartPosition() const{ return m_startPosition; }
    inline bool StartPositionHasBeenSet() const { return m_startPositionHasBeenSet; }
    inline void SetStartPosition(const Position& value) { m_startPositionHasBeenSet = true; m_startPosition = value; }
    inline void SetStartPosition(Position&& value) { m_startPositionHasBeenSet = true; m_startPosition = std::move(value); }
    inline Statement& WithStartPosition(const Position& value) { SetStartPosition(value); return *this;}
    inline Statement& WithStartPosition(Position&& value) { SetStartPosition(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The row and column of the end of a <code>Statement</code> in an IAM
     * policy.</p>
     */
    inline const Position& GetEndPosition() const{ return m_endPosition; }
    inline bool EndPositionHasBeenSet() const { return m_endPositionHasBeenSet; }
    inline void SetEndPosition(const Position& value) { m_endPositionHasBeenSet = true; m_endPosition = value; }
    inline void SetEndPosition(Position&& value) { m_endPositionHasBeenSet = true; m_endPosition = std::move(value); }
    inline Statement& WithEndPosition(const Position& value) { SetEndPosition(value); return *this;}
    inline Statement& WithEndPosition(Position&& value) { SetEndPosition(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_sourcePolicyId;
    bool m_sourcePolicyIdHasBeenSet = false;

    PolicySourceType m_sourcePolicyType;
    bool m_sourcePolicyTypeHasBeenSet = false;

    Position m_startPosition;
    bool m_startPositionHasBeenSet = false;

    Position m_endPosition;
    bool m_endPositionHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
