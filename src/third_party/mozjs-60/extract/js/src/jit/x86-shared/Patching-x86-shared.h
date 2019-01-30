/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_Patching_x86_shared_h
#define jit_x86_shared_Patching_x86_shared_h

namespace js {
namespace jit {

namespace X86Encoding {

inline void*
GetPointer(const void* where)
{
    void* res;
    memcpy(&res, (const char*)where - sizeof(void*), sizeof(void*));
    return res;
}

inline void
SetPointer(void* where, const void* value)
{
    memcpy((char*)where - sizeof(void*), &value, sizeof(void*));
}

inline int32_t
GetInt32(const void* where)
{
    int32_t res;
    memcpy(&res, (const char*)where - sizeof(int32_t), sizeof(int32_t));
    return res;
}

inline void
SetInt32(void* where, int32_t value)
{
    memcpy((char*)where - sizeof(int32_t), &value, sizeof(int32_t));
}

inline void
SetRel32(void* from, void* to)
{
    intptr_t offset = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from);
    MOZ_ASSERT(offset == static_cast<int32_t>(offset),
               "offset is too great for a 32-bit relocation");
    if (offset != static_cast<int32_t>(offset))
        MOZ_CRASH("offset is too great for a 32-bit relocation");

    SetInt32(from, offset);
}

inline void*
GetRel32Target(void* where)
{
    int32_t rel = GetInt32(where);
    return (char*)where + rel;
}

class JmpSrc {
  public:
    JmpSrc()
      : offset_(-1)
    {
    }
    explicit JmpSrc(int32_t offset)
      : offset_(offset)
    {
    }
    int32_t offset() const {
        return offset_;
    }
  private:
    int32_t offset_;
};

class JmpDst {
  public:
    explicit JmpDst(int32_t offset)
      : offset_(offset)
    {}
    int32_t offset() const {
        return offset_;
    }
  private:
    int32_t offset_;
};

inline bool
CanRelinkJump(void* from, void* to)
{
    intptr_t offset = static_cast<char*>(to) - static_cast<char*>(from);
    return (offset == static_cast<int32_t>(offset));
}

} // namespace X86Encoding

} // namespace jit
} // namespace js

#endif /* jit_x86_shared_Patching_x86_shared_h */
