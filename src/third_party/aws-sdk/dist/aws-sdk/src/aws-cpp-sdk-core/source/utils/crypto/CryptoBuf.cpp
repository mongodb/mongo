/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/crypto/CryptoBuf.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            SymmetricCryptoBufSrc::SymmetricCryptoBufSrc(Aws::IStream& stream, SymmetricCipher& cipher, CipherMode cipherMode, size_t bufferSize)
                    :
                    m_isBuf(PUT_BACK_SIZE), m_cipher(cipher), m_stream(stream), m_cipherMode(cipherMode), m_isFinalized(false),
                    m_bufferSize(bufferSize), m_putBack(PUT_BACK_SIZE)
            {
                char* end = reinterpret_cast<char*>(m_isBuf.GetUnderlyingData() + m_isBuf.GetLength());
                setg(end, end, end);
            }

            SymmetricCryptoBufSrc::pos_type SymmetricCryptoBufSrc::seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which)
            {
                if(which == std::ios_base::in)
                {
                    auto curPos = m_stream.tellg();
                    //error on seek we may have read past the end already. Try resetting and seeking to the end first
                    if (curPos == pos_type(-1))
                    {
                        m_stream.clear();
                        m_stream.seekg(0, std::ios_base::end);
                        curPos = m_stream.tellg();
                    }

                    auto absPosition = ComputeAbsSeekPosition(off, dir, curPos);
                    size_t seekTo = static_cast<size_t>(absPosition);
                    size_t index = static_cast<size_t>(curPos);

                    if(index == seekTo)
                    {
                        return curPos;
                    }
                    else if (seekTo < index)
                    {
                        m_cipher.Reset();
                        m_stream.clear();
                        m_stream.seekg(0);
                        m_isFinalized = false;
                        index = 0;
                    }

                    CryptoBuffer cryptoBuffer;
                    while (m_cipher && index < seekTo && !m_isFinalized)
                    {
                        size_t max_read = std::min<size_t>(static_cast<size_t>(seekTo - index), m_bufferSize);

                        Aws::Utils::Array<char> buf(max_read);
                        size_t readSize(0);
                        if(m_stream)
                        {
                            m_stream.read(buf.GetUnderlyingData(), max_read);
                            readSize = static_cast<size_t>(m_stream.gcount());
                        }

                        if (readSize > 0)
                        {
                            if (m_cipherMode == CipherMode::Encrypt)
                            {
                                cryptoBuffer = m_cipher.EncryptBuffer(CryptoBuffer(reinterpret_cast<unsigned char*>(buf.GetUnderlyingData()), readSize));
                            }
                            else
                            {
                                cryptoBuffer = m_cipher.DecryptBuffer(CryptoBuffer(reinterpret_cast<unsigned char*>(buf.GetUnderlyingData()), readSize));
                            }
                        }
                        else
                        {
                            if (m_cipherMode == CipherMode::Encrypt)
                            {
                                cryptoBuffer = m_cipher.FinalizeEncryption();
                            }
                            else
                            {
                                cryptoBuffer = m_cipher.FinalizeDecryption();
                            }

                            m_isFinalized = true;
                        }

                        index += cryptoBuffer.GetLength();
                    }

                    if (cryptoBuffer.GetLength() && m_cipher)
                    {
                        CryptoBuffer putBackArea(m_putBack);

                        m_isBuf = CryptoBuffer({&putBackArea, &cryptoBuffer});
                        //in the very unlikely case that the cipher had less output than the source stream.
                        assert(seekTo <= index);
                        size_t newBufferPos = index > seekTo ? cryptoBuffer.GetLength() - (index - seekTo) : cryptoBuffer.GetLength();
                        char* baseBufPtr = reinterpret_cast<char*>(m_isBuf.GetUnderlyingData());
                        setg(baseBufPtr, baseBufPtr + m_putBack + newBufferPos, baseBufPtr + m_isBuf.GetLength());

                        return pos_type(seekTo);
                    }
                    else if (seekTo == 0)
                    {
                        m_isBuf = CryptoBuffer(m_putBack);
                        char* end = reinterpret_cast<char*>(m_isBuf.GetUnderlyingData() + m_isBuf.GetLength());
                        setg(end, end, end);
                        return pos_type(seekTo);
                    }
                }

                return pos_type(off_type(-1));
            }

            SymmetricCryptoBufSrc::pos_type SymmetricCryptoBufSrc::seekpos(pos_type pos, std::ios_base::openmode which)
            {
                return seekoff(pos, std::ios_base::beg, which);
            }

            SymmetricCryptoBufSrc::int_type SymmetricCryptoBufSrc::underflow()
            {
                if (!m_cipher || (m_isFinalized && gptr() >= egptr()))
                {
                    return traits_type::eof();
                }

                if (gptr() < egptr())
                {
                    return traits_type::to_int_type(*gptr());
                }

                char* baseBufPtr = reinterpret_cast<char*>(m_isBuf.GetUnderlyingData());
                CryptoBuffer putBackArea(m_putBack);

                //eback is properly set after the first fill. So this guarantees we are on the second or later fill.
                if (eback() == baseBufPtr)
                {
                    //just fill in the last bit of the previous buffer into the put back area so that it has some data in it
                    memcpy(putBackArea.GetUnderlyingData(), egptr() - m_putBack, m_putBack);
                }

                CryptoBuffer newDataBuf;

                while(!newDataBuf.GetLength() && !m_isFinalized)
                {
                    Aws::Utils::Array<char> buf(m_bufferSize);
                    m_stream.read(buf.GetUnderlyingData(), m_bufferSize);
                    size_t readSize = static_cast<size_t>(m_stream.gcount());

                    if (readSize > 0)
                    {
                        if (m_cipherMode == CipherMode::Encrypt)
                        {
                            newDataBuf = m_cipher.EncryptBuffer(CryptoBuffer(reinterpret_cast<unsigned char*>(buf.GetUnderlyingData()), readSize));
                        }
                        else
                        {
                            newDataBuf = m_cipher.DecryptBuffer(CryptoBuffer(reinterpret_cast<unsigned char*>(buf.GetUnderlyingData()), readSize));
                        }
                    }
                    else
                    {
                        if (m_cipherMode == CipherMode::Encrypt)
                        {
                            newDataBuf = m_cipher.FinalizeEncryption();
                        }
                        else
                        {
                            newDataBuf = m_cipher.FinalizeDecryption();
                        }

                        m_isFinalized = true;
                    }
                }


                if(newDataBuf.GetLength() > 0)
                {
                    m_isBuf = CryptoBuffer({&putBackArea, &newDataBuf});

                    baseBufPtr = reinterpret_cast<char*>(m_isBuf.GetUnderlyingData());
                    setg(baseBufPtr, baseBufPtr + m_putBack, baseBufPtr + m_isBuf.GetLength());

                    return traits_type::to_int_type(*gptr());
                }

                return traits_type::eof();
            }

            SymmetricCryptoBufSrc::off_type SymmetricCryptoBufSrc::ComputeAbsSeekPosition(off_type pos, std::ios_base::seekdir dir,  std::fpos<FPOS_TYPE> curPos)
            {
                switch(dir)
                {
                case std::ios_base::beg:
                    return pos;
                case std::ios_base::cur:
                    return m_stream.tellg() + pos;
                case std::ios_base::end:
                {
                    off_type absPos = m_stream.seekg(0, std::ios_base::end).tellg() - pos;
                    m_stream.seekg(curPos);
                    return absPos;
                }
                default:
                    assert(0);
                    return off_type(-1);
                }
            }

            void SymmetricCryptoBufSrc::FinalizeCipher()
            {
                if(m_cipher && !m_isFinalized)
                {
                    if(m_cipherMode == CipherMode::Encrypt)
                    {
                        m_cipher.FinalizeEncryption();
                    }
                    else
                    {
                        m_cipher.FinalizeDecryption();
                    }
                }
            }

            SymmetricCryptoBufSink::SymmetricCryptoBufSink(Aws::OStream& stream, SymmetricCipher& cipher, CipherMode cipherMode, size_t bufferSize, int16_t blockOffset)
                    :
                    m_osBuf(bufferSize), m_cipher(cipher), m_stream(stream), m_cipherMode(cipherMode), m_isFinalized(false), m_blockOffset(blockOffset)
            {
                assert(m_blockOffset < 16 && m_blockOffset >= 0);
                char* outputBase = reinterpret_cast<char*>(m_osBuf.GetUnderlyingData());
                setp(outputBase, outputBase + bufferSize - 1);
            }

            SymmetricCryptoBufSink::~SymmetricCryptoBufSink()
            {
                FinalizeCiphersAndFlushSink();
            }

            void SymmetricCryptoBufSink::FinalizeCiphersAndFlushSink()
            {
                if(m_cipher && !m_isFinalized)
                {
                    writeOutput(true);
                }
            }

            bool SymmetricCryptoBufSink::writeOutput(bool finalize)
            {
                if(!m_isFinalized)
                {
                    CryptoBuffer cryptoBuf;
                    if (pptr() > pbase())
                    {
                        if (m_cipherMode == CipherMode::Encrypt)
                        {
                            cryptoBuf = m_cipher.EncryptBuffer(CryptoBuffer(reinterpret_cast<unsigned char*>(pbase()), pptr() - pbase()));
                        }
                        else
                        {
                            cryptoBuf = m_cipher.DecryptBuffer(CryptoBuffer(reinterpret_cast<unsigned char*>(pbase()), pptr() - pbase()));
                        }

                        pbump(-(static_cast<int>(pptr() - pbase())));
                    }
                    if(finalize)
                    {
                        CryptoBuffer finalBuffer;
                        if (m_cipherMode == CipherMode::Encrypt)
                        {
                            finalBuffer = m_cipher.FinalizeEncryption();
                        }
                        else
                        {
                            finalBuffer = m_cipher.FinalizeDecryption();
                        }
                        if(cryptoBuf.GetLength())
                        {
                            cryptoBuf = CryptoBuffer({&cryptoBuf, &finalBuffer});
                        }
                        else
                        {
                            cryptoBuf = std::move(finalBuffer);
                        }

                        m_isFinalized = true;
                    }

                    if(cryptoBuf.GetLength())
                    {
                        //allow mid block decryption. We have to decrypt it, but we don't have to write it to the stream.
                        //the assumption here is that tellp() will always be 0 or >= 16 bytes. The block offset should only
                        //be the offset of the first block read.
                        size_t len = cryptoBuf.GetLength();
                        size_t blockOffset = m_stream.good() && m_stream.tellp() > m_blockOffset ? 0 : m_blockOffset;
                        if (len > blockOffset)
                        {
                            m_stream.write(reinterpret_cast<char*>(cryptoBuf.GetUnderlyingData() + blockOffset), len - blockOffset);
                            m_blockOffset = 0;
                        }
                        else
                        {
                            m_blockOffset -= static_cast<int16_t>(len);
                        }
                    }
                    return true;
                }

                return false;
            }

            SymmetricCryptoBufSink::int_type SymmetricCryptoBufSink::overflow(int_type ch)
            {
                if(m_cipher && m_stream)
                {
                    if(ch != traits_type::eof())
                    {
                        *pptr() = (char)ch;
                        pbump(1);
                    }

                    if(writeOutput(ch == traits_type::eof()))
                    {
                        return ch;
                    }
                }

                return traits_type::eof();
            }

            int SymmetricCryptoBufSink::sync()
            {
                if(m_cipher && m_stream)
                {
                    return writeOutput(false) ? 0 : -1;
                }

                return -1;
            }
        }
    }
}
