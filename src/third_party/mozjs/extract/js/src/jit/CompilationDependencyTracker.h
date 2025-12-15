/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CompilationDependencyTracker_h
#define jit_CompilationDependencyTracker_h

#include "mozilla/Vector.h"

#include "jstypes.h"
#include "NamespaceImports.h"

struct JSContext;

namespace js::jit {
class MIRGenerator;

struct CompilationDependency {
  enum class Type {
    GetIterator,
    ArraySpecies,
    RegExpPrototype,
    StringPrototypeSymbols,
    EmulatesUndefined,
    ArrayExceedsInt32Length,
    Limit
  };

  Type type;

  CompilationDependency(Type type) : type(type) {}
  virtual bool operator==(const CompilationDependency& other) const = 0;

  // Return true iff this dependency still holds. May only be called on main
  // thread.
  virtual bool checkDependency(JSContext* cx) = 0;
  [[nodiscard]] virtual bool registerDependency(JSContext* cx,
                                                HandleScript script) = 0;

  virtual UniquePtr<CompilationDependency> clone() const = 0;
  virtual ~CompilationDependency() = default;
};

// For a given Warp compilation keep track of the dependencies this compilation
// is depending on. These dependencies will be checked on main thread during
// link time, causing abandonment of a compilation if they no longer hold.
struct CompilationDependencyTracker {
  mozilla::Vector<UniquePtr<CompilationDependency>, 8, SystemAllocPolicy>
      dependencies;

  [[nodiscard]] bool addDependency(const CompilationDependency& dep) {
    // Ensure we don't add duplicates. We expect this list to be short,
    // and so iteration is preferred over a more costly hashset.
    MOZ_ASSERT(dependencies.length() <= 32);
    for (auto& existingDep : dependencies) {
      if (dep == *existingDep) {
        return true;
      }
    }

    auto clone = dep.clone();
    if (!clone) {
      return false;
    }

    return dependencies.append(std::move(clone));
  }

  // Check all dependencies. May only be checked on main thread.
  bool checkDependencies(JSContext* cx) {
    for (auto& dep : dependencies) {
      if (!dep->checkDependency(cx)) {
        return false;
      }
    }
    return true;
  }

  void reset() { dependencies.clearAndFree(); }
};

}  // namespace js::jit
#endif /* jit_CompilationDependencyTracker_h */
