/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_ScopedICUObject_h
#define builtin_intl_ScopedICUObject_h

/*
 * A simple RAII class to assure ICU objects are automatically deallocated at
 * scope end.  Unfortunately, ICU's C++ API is uniformly unstable, so we can't
 * use its smart pointers for this.
 */

namespace js {

template<typename T, void (Delete)(T*)>
class ScopedICUObject
{
    T* ptr_;

  public:
    explicit ScopedICUObject(T* ptr)
      : ptr_(ptr)
    {}

    ~ScopedICUObject() {
        if (ptr_)
            Delete(ptr_);
    }

    // In cases where an object should be deleted on abnormal exits,
    // but returned to the caller if everything goes well, call forget()
    // to transfer the object just before returning.
    T* forget() {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }
};

} // namespace js

#endif /* builtin_intl_ScopedICUObject_h */
