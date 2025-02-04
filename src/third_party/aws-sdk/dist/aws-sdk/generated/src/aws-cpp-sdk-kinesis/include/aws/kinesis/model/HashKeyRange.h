/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
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
namespace Kinesis
{
namespace Model
{

  /**
   * <p>The range of possible hash key values for the shard, which is a set of
   * ordered contiguous positive integers.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/HashKeyRange">AWS
   * API Reference</a></p>
   */
  class HashKeyRange
  {
  public:
    AWS_KINESIS_API HashKeyRange();
    AWS_KINESIS_API HashKeyRange(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API HashKeyRange& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The starting hash key of the hash key range.</p>
     */
    inline const Aws::String& GetStartingHashKey() const{ return m_startingHashKey; }
    inline bool StartingHashKeyHasBeenSet() const { return m_startingHashKeyHasBeenSet; }
    inline void SetStartingHashKey(const Aws::String& value) { m_startingHashKeyHasBeenSet = true; m_startingHashKey = value; }
    inline void SetStartingHashKey(Aws::String&& value) { m_startingHashKeyHasBeenSet = true; m_startingHashKey = std::move(value); }
    inline void SetStartingHashKey(const char* value) { m_startingHashKeyHasBeenSet = true; m_startingHashKey.assign(value); }
    inline HashKeyRange& WithStartingHashKey(const Aws::String& value) { SetStartingHashKey(value); return *this;}
    inline HashKeyRange& WithStartingHashKey(Aws::String&& value) { SetStartingHashKey(std::move(value)); return *this;}
    inline HashKeyRange& WithStartingHashKey(const char* value) { SetStartingHashKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ending hash key of the hash key range.</p>
     */
    inline const Aws::String& GetEndingHashKey() const{ return m_endingHashKey; }
    inline bool EndingHashKeyHasBeenSet() const { return m_endingHashKeyHasBeenSet; }
    inline void SetEndingHashKey(const Aws::String& value) { m_endingHashKeyHasBeenSet = true; m_endingHashKey = value; }
    inline void SetEndingHashKey(Aws::String&& value) { m_endingHashKeyHasBeenSet = true; m_endingHashKey = std::move(value); }
    inline void SetEndingHashKey(const char* value) { m_endingHashKeyHasBeenSet = true; m_endingHashKey.assign(value); }
    inline HashKeyRange& WithEndingHashKey(const Aws::String& value) { SetEndingHashKey(value); return *this;}
    inline HashKeyRange& WithEndingHashKey(Aws::String&& value) { SetEndingHashKey(std::move(value)); return *this;}
    inline HashKeyRange& WithEndingHashKey(const char* value) { SetEndingHashKey(value); return *this;}
    ///@}
  private:

    Aws::String m_startingHashKey;
    bool m_startingHashKeyHasBeenSet = false;

    Aws::String m_endingHashKey;
    bool m_endingHashKeyHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
