/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/memory/stl/SimpleStringStream.h>

namespace Aws
{

SimpleStringStream::SimpleStringStream() :
    base(&m_streamBuffer),
    m_streamBuffer()
{
}

SimpleStringStream::SimpleStringStream(const Aws::String& value) :
    base(&m_streamBuffer),
    m_streamBuffer(value)
{
}

void SimpleStringStream::str(const Aws::String& value)
{
    m_streamBuffer.str(value);
}

//

SimpleIStringStream::SimpleIStringStream() :
    base(&m_streamBuffer),
    m_streamBuffer()
{
}

SimpleIStringStream::SimpleIStringStream(const Aws::String& value) :
    base(&m_streamBuffer),
    m_streamBuffer(value)
{
}

void SimpleIStringStream::str(const Aws::String& value)
{
    m_streamBuffer.str(value);
}

//

SimpleOStringStream::SimpleOStringStream() :
    base(&m_streamBuffer),
    m_streamBuffer()
{
}

SimpleOStringStream::SimpleOStringStream(const Aws::String& value) :
    base(&m_streamBuffer),
    m_streamBuffer(value)
{
}

void SimpleOStringStream::str(const Aws::String& value)
{
    m_streamBuffer.str(value);
}

} // namespace Aws
