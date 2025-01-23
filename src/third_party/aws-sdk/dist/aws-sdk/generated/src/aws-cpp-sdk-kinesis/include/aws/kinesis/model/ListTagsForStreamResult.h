/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/kinesis/model/Tag.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Kinesis
{
namespace Model
{
  /**
   * <p>Represents the output for <code>ListTagsForStream</code>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/ListTagsForStreamOutput">AWS
   * API Reference</a></p>
   */
  class ListTagsForStreamResult
  {
  public:
    AWS_KINESIS_API ListTagsForStreamResult();
    AWS_KINESIS_API ListTagsForStreamResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API ListTagsForStreamResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>A list of tags associated with <code>StreamName</code>, starting with the
     * first tag after <code>ExclusiveStartTagKey</code> and up to the specified
     * <code>Limit</code>. </p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tags = std::move(value); }
    inline ListTagsForStreamResult& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline ListTagsForStreamResult& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline ListTagsForStreamResult& AddTags(const Tag& value) { m_tags.push_back(value); return *this; }
    inline ListTagsForStreamResult& AddTags(Tag&& value) { m_tags.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>If set to <code>true</code>, more tags are available. To request additional
     * tags, set <code>ExclusiveStartTagKey</code> to the key of the last tag
     * returned.</p>
     */
    inline bool GetHasMoreTags() const{ return m_hasMoreTags; }
    inline void SetHasMoreTags(bool value) { m_hasMoreTags = value; }
    inline ListTagsForStreamResult& WithHasMoreTags(bool value) { SetHasMoreTags(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListTagsForStreamResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListTagsForStreamResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListTagsForStreamResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<Tag> m_tags;

    bool m_hasMoreTags;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
