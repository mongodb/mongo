/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/UbiNodeShortestPaths.h"

#include "mozilla/Maybe.h"
#include "mozilla/Move.h"

#include "builtin/String.h"
#include "util/Text.h"

namespace JS {
namespace ubi {

JS_PUBLIC_API(BackEdge::Ptr)
BackEdge::clone() const
{
    BackEdge::Ptr clone(js_new<BackEdge>());
    if (!clone)
        return nullptr;

    clone->predecessor_ = predecessor();
    if (name()) {
        clone->name_ = js::DuplicateString(name().get());
        if (!clone->name_)
            return nullptr;
    }
    return mozilla::Move(clone);
}

#ifdef DEBUG

static void
dumpNode(const JS::ubi::Node& node)
{
    fprintf(stderr, "    %p ", (void*) node.identifier());
    js_fputs(node.typeName(), stderr);
    if (node.coarseType() == JS::ubi::CoarseType::Object) {
        if (const char* clsName = node.jsObjectClassName())
            fprintf(stderr, " [object %s]", clsName);
    }
    fputc('\n', stderr);
}

JS_PUBLIC_API(void)
dumpPaths(JSContext* cx, Node node, uint32_t maxNumPaths /* = 10 */)
{
    mozilla::Maybe<AutoCheckCannotGC> nogc;

    JS::ubi::RootList rootList(cx, nogc, true);
    MOZ_ASSERT(rootList.init());

    NodeSet targets;
    bool ok = targets.init() && targets.putNew(node);
    MOZ_ASSERT(ok);

    auto paths = ShortestPaths::Create(cx, nogc.ref(), maxNumPaths, &rootList, mozilla::Move(targets));
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
            if (!name)
                name = u"<no edge name>";
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

    if (i == 0)
        fprintf(stderr, "No retaining paths found.\n");
}
#endif

} // namespace ubi
} // namespace JS
