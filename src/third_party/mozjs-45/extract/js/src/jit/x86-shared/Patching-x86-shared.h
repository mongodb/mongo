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
    return reinterpret_cast<void* const*>(where)[-1];
}

inline void**
GetPointerRef(void* where)
{
    return &reinterpret_cast<void**>(where)[-1];
}

inline void
SetPointer(void* where, const void* value)
{
    reinterpret_cast<const void**>(where)[-1] = value;
}

inline int32_t
GetInt32(const void* where)
{
    return reinterpret_cast<const int32_t*>(where)[-1];
}

inline void
SetInt32(void* where, int32_t value)
{
    reinterpret_cast<int32_t*>(where)[-1] = value;
}

inline void
AddInt32(void* where, int32_t value)
{
#ifdef DEBUG
    uint32_t x = reinterpret_cast<uint32_t*>(where)[-1];
    uint32_t y = x + uint32_t(value);
    MOZ_ASSERT(value >= 0 ? (int32_t(y) >= int32_t(x)) : (int32_t(y) < int32_t(x)));
#endif
    reinterpret_cast<uint32_t*>(where)[-1] += uint32_t(value);
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

    bool isSet() const {
        return offset_ != -1;
    }

  private:
    int offset_;
};

class JmpDst {
  public:
    JmpDst()
        : offset_(-1)
        , used_(false)
    {
    }

    bool isUsed() const { return used_; }
    void used() { used_ = true; }
    bool isValid() const { return offset_ != -1; }

    explicit JmpDst(int32_t offset)
        : offset_(offset)
        , used_(false)
    {
        MOZ_ASSERT(offset_ == offset);
    }
    int32_t offset() const {
        return offset_;
    }
  private:
    int32_t offset_ : 31;
    bool used_ : 1;
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
