/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/UbiNodeShortestPaths.h"

#include "mozilla/Maybe.h"

#include <stdio.h>
#include <utility>

#include "util/Text.h"

namespace JS {
namespace ubi {

JS_PUBLIC_API BackEdge::Ptr BackEdge::clone() const {
  auto clone = js::MakeUnique<BackEdge>();
  if (!clone) {
    return nullptr;
  }

  clone->predecessor_ = predecessor();
  if (name()) {
    clone->name_ = js::DuplicateString(name().get());
    if (!clone->name_) {
      return nullptr;
    }
  }
  return clone;
}

#ifdef DEBUG

static int32_t js_fputs(const char16_t* s, FILE* f) {
  while (*s != 0) {
    if (fputwc(wchar_t(*s), f) == static_cast<wint_t>(WEOF)) {
      return WEOF;
    }
    s++;
  }
  return 1;
}

static void dumpNode(const JS::ubi::Node& node) {
  fprintf(stderr, "    %p ", (void*)node.identifier());
  js_fputs(node.typeName(), stderr);
  if (node.coarseType() == JS::ubi::CoarseType::Object) {
    if (const char* clsName = node.jsObjectClassName()) {
      fprintf(stderr, " [object %s]", clsName);
    }
  }
  fputc('\n', stderr);
}

JS_PUBLIC_API void dumpPaths(JSContext* cx, Node node,
                             uint32_t maxNumPaths /* = 10 */) {
  JS::ubi::RootList rootList(cx, true);
  auto [ok, nogc] = rootList.init();
  MOZ_ASSERT(ok);

  NodeSet targets;
  ok = targets.putNew(node);
  MOZ_ASSERT(ok);

  auto paths = ShortestPaths::Create(cx, nogc, maxNumPaths, &rootList,
                                     std::move(targets));
  MOZ_ASSERT(paths.isSome());

  int i = 0;
  ok = paths->forEachPath(node, [&](Path& path) {
    fprintf(stderr, "Path %d:\n", i++);
    for (auto backEdge : path) {
      dumpNode(backEdge->predecessor());
      fprintf(stderr, "            |\n");
      fprintf(stderr, "            |\n");
      fprintf(stderr, "        '");

      const char16_t* name = backEdge->name().get();
      if (!name) {
        name = u"<no edge name>";
      }
      js_fputs(name, stderr);
      fprintf(stderr, "'\n");

      fprintf(stderr, "            |\n");
      fprintf(stderr, "            V\n");
    }

    dumpNode(node);
    fputc('\n', stderr);
    return true;
  });
  MOZ_ASSERT(ok);

  if (i == 0) {
    fprintf(stderr, "No retaining paths found.\n");
  }
}
#endif

}  // namespace ubi
}  // namespace JS
