/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Similar to std::ifstream/ofstream, but takes char16ptr_t on Windows.
// Until C++17, std functions can only take char* filenames. So Unicode
// filenames were lost on Windows. To address this limitations, this wrapper
// uses proprietary wchar_t* overloads on MSVC, and __gnu_cxx::stdio_filebuf
// extension on MinGW. Once we can use C++17 filesystem API everywhere,
// we will be able to avoid this wrapper.

#ifndef mozilla_FStream_h
#define mozilla_FStream_h

#include "mozilla/Char16.h"
#include <istream>
#include <ostream>
#include <fstream>
#if defined(__MINGW32__) && defined(__GLIBCXX__)
#include "mozilla/UniquePtr.h"
#include <fcntl.h>
#include <ext/stdio_filebuf.h>
#endif

namespace mozilla {

#if defined(__MINGW32__) && defined(__GLIBCXX__)
// MinGW does not support wchar_t* overloads that are MSVC extension until
// C++17, so we have to implement widechar wrappers using a GNU extension.
class IFStream : public std::istream
{
public:
  explicit IFStream(char16ptr_t filename, openmode mode = in);

  std::filebuf* rdbuf() const { return mFileBuf.get(); }

  bool is_open() const { return mFileBuf && mFileBuf->is_open(); }
  void open(char16ptr_t filename, openmode mode = in);
  void close() { mFileBuf && mFileBuf->close(); }

private:
  UniquePtr<std::filebuf> mFileBuf;
};

inline
IFStream::IFStream(char16ptr_t filename, openmode mode)
  : std::istream(nullptr)
{
  open(filename, mode);
}

inline void
IFStream::open(char16ptr_t filename, openmode mode)
{
  int fmode = _O_RDONLY;
  if (mode & binary) {
    fmode |= _O_BINARY;
  } else {
    fmode |= _O_TEXT;
  }
  int fd = _wopen(filename, fmode);
  mFileBuf = MakeUnique<__gnu_cxx::stdio_filebuf<char>>(fd, mode);
  std::istream::rdbuf(mFileBuf.get());
}

class OFStream : public std::ostream
{
public:
  explicit OFStream(char16ptr_t filename, openmode mode = out);

  std::filebuf* rdbuf() const { return mFileBuf.get(); }

  bool is_open() const { return mFileBuf && mFileBuf->is_open(); }
  void open(char16ptr_t filename, openmode mode = out);
  void close() { mFileBuf && mFileBuf->close(); }

private:
  UniquePtr<std::filebuf> mFileBuf;
};

inline
OFStream::OFStream(char16ptr_t filename, openmode mode)
  : std::ostream(nullptr)
{
  open(filename, mode);
}

inline void
OFStream::open(char16ptr_t filename, openmode mode)
{
  int fmode = _O_WRONLY;
  if (mode & binary) {
    fmode |= _O_BINARY;
  } else {
    fmode |= _O_TEXT;
  }
  if (mode & trunc) {
    fmode |= _O_CREAT | _O_TRUNC;
  }
  int fd = _wopen(filename, fmode);
  mFileBuf = MakeUnique<__gnu_cxx::stdio_filebuf<char>>(fd, mode);
  std::ostream::rdbuf(mFileBuf.get());
}

#elif defined(XP_WIN)
class IFStream : public std::ifstream
{
public:
  explicit IFStream(char16ptr_t filename, openmode mode = in)
    : std::ifstream(filename, mode) {}

  void open(char16ptr_t filename, openmode mode = in)
  {
    std::ifstream::open(filename, mode);
  }
};

class OFStream : public std::ofstream
{
public:
  explicit OFStream(char16ptr_t filename, openmode mode = out)
    : std::ofstream(filename, mode) {}

  void open(char16ptr_t filename, openmode mode = out)
  {
    std::ofstream::open(filename, mode);
  }
};
#else
using IFStream = std::ifstream;
using OFStream = std::ofstream;
#endif

} // namespace mozilla

#endif /* mozilla_FStream_h */
