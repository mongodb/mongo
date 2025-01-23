/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/stream/ResponseStream.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/logging/LogMacros.h>

#if defined(_GLIBCXX_FULLY_DYNAMIC_STRING) && _GLIBCXX_FULLY_DYNAMIC_STRING == 0 && defined(__ANDROID__)
#include <aws/core/utils/stream/SimpleStreamBuf.h>
using DefaultStreamBufType = Aws::Utils::Stream::SimpleStreamBuf;
#else
using DefaultStreamBufType = Aws::StringBuf;
#endif

using namespace Aws::Utils::Stream;

const int ResponseStream::ResponseStream::xindex = std::ios_base::xalloc();

ResponseStream::ResponseStream(void) :
    m_underlyingStream(nullptr)
{
}

ResponseStream::ResponseStream(Aws::IOStream* underlyingStreamToManage) :
    m_underlyingStream(underlyingStreamToManage)
{
    RegisterStream();
}

ResponseStream::ResponseStream(const Aws::IOStreamFactory& factory) :
    m_underlyingStream(factory())
{
    RegisterStream();
}

ResponseStream::ResponseStream(ResponseStream&& toMove) : m_underlyingStream(toMove.m_underlyingStream)
{
    toMove.DeregisterStream();
    toMove.m_underlyingStream = nullptr;
    RegisterStream();
}

ResponseStream& ResponseStream::operator=(ResponseStream&& toMove)
{
    if(m_underlyingStream == toMove.m_underlyingStream)
    {
        return *this;
    }

    ReleaseStream();
    toMove.DeregisterStream();
    m_underlyingStream = toMove.m_underlyingStream;
    toMove.m_underlyingStream = nullptr;
    RegisterStream();

    return *this;
}

Aws::IOStream& ResponseStream::GetUnderlyingStream() const
{
    if (!m_underlyingStream)
    {
        assert(m_underlyingStream);
        AWS_LOGSTREAM_FATAL("ResponseStream", "Unexpected nullptr m_underlyingStream");
        static DefaultUnderlyingStream fallbackStream; // we are already in UB, let's just not crash existing apps
        return fallbackStream;
    }
    return *m_underlyingStream;
}

ResponseStream::~ResponseStream()
{
    ReleaseStream();
}

void ResponseStream::ReleaseStream()
{
    if (m_underlyingStream)
    {
        DeregisterStream();
        Aws::Delete(m_underlyingStream);
    }

    m_underlyingStream = nullptr;
}

void ResponseStream::RegisterStream()
{
    if (m_underlyingStream)
    {
        ResponseStream* pThat = static_cast<ResponseStream*>(m_underlyingStream->pword(ResponseStream::xindex));
        if (pThat != nullptr)
        {
            // callback is already registered
            assert(pThat != this); // Underlying stream must not be owned by more than one ResponseStream
        }
        else
        {
            m_underlyingStream->register_callback(ResponseStream::StreamCallback, ResponseStream::xindex);
        }
        m_underlyingStream->pword(ResponseStream::xindex) = this;
    }
}

void ResponseStream::DeregisterStream()
{
    if (m_underlyingStream)
    {
        assert(static_cast<ResponseStream*>(m_underlyingStream->pword(ResponseStream::xindex)) == this); // Attempt to deregister another ResponseStream's stream
        m_underlyingStream->pword(ResponseStream::xindex) = nullptr; // ios does not support deregister, so just erasing the context
    }
}

void ResponseStream::StreamCallback(Aws::IOStream::event evt, std::ios_base& stream, int idx)
{
    if (evt == std::ios_base::erase_event)
    {
        ResponseStream* pThis = static_cast<ResponseStream*>(stream.pword(idx));
        if (pThis)
        {
            // m_underlyingStream is being destructed, let's avoid double destruction or having a dangling pointer
            pThis->m_underlyingStream = nullptr;
        }
    }
}

static const char *DEFAULT_STREAM_TAG = "DefaultUnderlyingStream";

DefaultUnderlyingStream::DefaultUnderlyingStream() :
    Base( Aws::New< DefaultStreamBufType >( DEFAULT_STREAM_TAG ) )
{}

DefaultUnderlyingStream::DefaultUnderlyingStream(Aws::UniquePtr<std::streambuf> buf) :
    Base(buf.release())
{}

DefaultUnderlyingStream::~DefaultUnderlyingStream()
{
    if( rdbuf() )
    {
        Aws::Delete( rdbuf() );
    }
}

static const char* RESPONSE_STREAM_FACTORY_TAG = "ResponseStreamFactory";

Aws::IOStream* Aws::Utils::Stream::DefaultResponseStreamFactoryMethod() 
{
    return Aws::New<Aws::Utils::Stream::DefaultUnderlyingStream>(RESPONSE_STREAM_FACTORY_TAG);
}
