#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

# Self-test for s_tombstone_encoding.py, the layered tombstone value encoding lint. Feeds small
# in-memory snippets through check_text() and asserts the finding count, then runs the check against
# the real src/cursor/cur_layered.c as a golden regression guard against false positives. Silent and
# exit 0 on success so s_all stays clean; prints the mismatches and exits 1 on any failure.
#
# FIXME-WT-18244: reconsider the lint's value once value transfers go through encode/decode
# wrappers; this self-test goes wherever s_tombstone_encoding.py goes.

import os
import sys

# Importing the checker writes dist/__pycache__/s_tombstone_encoding.*.pyc, whose name matches
# s_whitespace's 's_*' file scan and trips it. Skip bytecode so the run leaves no such file.
sys.dont_write_bytecode = True

import s_tombstone_encoding as ste

FAILURES = []


def expect(name, text, count, contains=None):
    problems = ste.check_text(text)
    ok = len(problems) == count
    if contains is not None:
        ok = ok and any(contains in p for p in problems)
    if not ok:
        FAILURES.append((name, count, contains, problems))


def expect_ingest(name, text, count, contains=None):
    problems = ste.check_ingest_text(text)
    ok = len(problems) == count
    if contains is not None:
        ok = ok and any(contains in p for p in problems)
    if not ok:
        FAILURES.append((name, count, contains, problems))


def expect_prepared(name, text, count, contains=None):
    problems = ste.check_prepared_text(text)
    ok = len(problems) == count
    if contains is not None:
        ok = ok and any(contains in p for p in problems)
    if not ok:
        FAILURES.append((name, count, contains, problems))


# A promotion followed by a decode in the same function is correct: no finding.
expect("promotion with decode", """
static int
__clayered_reader(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;

    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);
    __clayered_deleted_decode(
      session, &cursor->value, clayered->current_cursor == clayered->stable_cursor);
    return (0);
}
""", 0)

# A promotion with no decode hands the caller escaped bytes.
expect("promotion without decode", """
static int
__clayered_bad_reader(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;

    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);
    return (0);
}
""", 1, contains="promoted to")

# The dominant read idiom: a lookup writing straight into cursor->value still owes a decode.
expect("lookup into cursor->value without decode", """
static int
__clayered_search(WT_CURSOR *cursor)
{
    WTI_CLAYERED_OP op;

    WT_ERR(__clayered_lookup(&op, &cursor->value));
    return (0);
}
""", 1, contains="looked up into")

# A write path looks a value up into a local WT_ITEM before re-encoding it; it must not be flagged.
expect("lookup into a local value is not a read", """
static int
__clayered_writer(WT_CURSOR *cursor)
{
    WTI_CLAYERED_OP op;
    WT_ITEM value;

    WT_ERR(__clayered_lookup(&op, &value));
    return (0);
}
""", 0)

# An encode call missing its final table decision argument.
expect("encode with wrong argument count", """
static int
__clayered_encoder(WT_CURSOR *cursor)
{
    WT_ITEM value, *buf;

    WT_ERR(__clayered_deleted_encode(session, &cursor->value, &value, &buf));
    return (0);
}
""", 1, contains="arguments, expected 5")

# A decode whose final argument is not a constituent decision.
expect("decode with a bad final argument", """
static int
__clayered_decoder(WT_CURSOR *cursor)
{
    __clayered_deleted_decode(session, &cursor->value, some_flag);
    return (0);
}
""", 1, contains="is not a")

# An encode whose constituent-decision (third) argument is not a table decision.
expect("encode with a bad decision argument", """
static int
__clayered_encoder2(WT_CURSOR *cursor)
{
    WT_ITEM value, *buf;

    WT_ERR(__clayered_deleted_encode(session, &cursor->value, whoops, &value, &buf));
    return (0);
}
""", 1, contains="is not a")

# A well-formed encode with the table decision as its third argument is correct.
expect("encode with a good decision argument", """
static int
__clayered_encoder3(WT_CURSOR *cursor)
{
    WT_ITEM value, *buf;

    WT_ERR(__clayered_deleted_encode(session, &cursor->value, op.ingest == NULL, &value, &buf));
    return (0);
}
""", 0)

# The __clayered_decode_current() wrapper satisfies the Rule 1 decode obligation.
expect("promotion with the decode wrapper", """
static int
__clayered_reader2(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;

    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);
    __clayered_decode_current(clayered, &cursor->value);
    return (0);
}
""", 0)

# A hand-rolled use of the raw marker outside the sanctioned helpers.
expect("raw marker in an unsanctioned function", """
static int
__clayered_sneaky(WT_CURSOR *cursor)
{
    memcpy(buf, __wt_tombstone.data, __wt_tombstone.size);
    return (0);
}
""", 1, contains="raw tombstone marker")

# The same raw marker use inside a sanctioned helper is allowed.
expect("raw marker in a sanctioned helper", """
static WT_INLINE int
__clayered_deleted_encode(WT_SESSION_IMPL *session, const WT_ITEM *value, bool to_stable,
  WT_ITEM *final_value, WT_ITEM **tmpp)
{
    memcpy((uint8_t *)tmp->mem + value->size, __wt_tombstone.data, 1);
    return (0);
}
""", 0)

# Writing the real marker to record a delete is a legitimate tombstone write, not a hand-rolled one.
expect("writing the real tombstone marker is exempt", """
static int
__clayered_delete(WT_CURSOR *cursor)
{
    cursor->set_value(cursor, &__wt_tombstone);
    return (0);
}
""", 0)

# The marker named only in a block comment must not trip the scan; line numbers stay aligned.
expect("marker mentioned only in a comment", """
/*
 * The reserved value __wt_tombstone is the two bytes {\\x14\\x14}.
 */
static int
__clayered_noop(WT_CURSOR *cursor)
{
    return (0);
}
""", 0)

# ALLOWED_MISSING_DECODE prints the documenting ticket instead of silently dropping the site.
skip_text = """
static int
__clayered_bad_reader(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;

    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);
    return (0);
}
"""
saved = ste.ALLOWED_MISSING_DECODE
ste.ALLOWED_MISSING_DECODE = {"__clayered_bad_reader": "intentional legacy skip"}
try:
    skips = []
    problems = ste.check_text(skip_text, skips)
    if problems or len(skips) != 1 or "intentional legacy skip" not in skips[0]:
        FAILURES.append(
            ("allow-listed skip prints its reason", 0, "intentional legacy skip",
             problems + skips))
finally:
    ste.ALLOWED_MISSING_DECODE = saved

# Golden regression: the real layered cursor must be clean, guarding against false positives.
real = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ste.TARGET))
if os.path.exists(real):
    real_problems = ste.check(real)
    if real_problems:
        FAILURES.append(("real cur_layered.c is clean", 0, None, real_problems))
else:
    FAILURES.append(("real cur_layered.c is present", 0, None, [f"missing {real}"]))

# Drain (conn_layered_ingest.c): a standard value converted to the stable form before the
# allocation is correct.
expect_ingest("drain converts before the standard allocation", """
static int
__layered_copy_ingest_table(WT_SESSION_IMPL *session)
{
    if (__wt_clayered_deleted(value))
        WT_ERR(__wt_upd_alloc_tombstone(session, &upd, NULL));
    else {
        __wt_clayered_ingest_to_stable_value(session, value);
        WT_ERR(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, NULL));
    }
    return (0);
}
""", 0)

# A standard value allocated with no conversion inherits the escape byte on the stable image.
expect_ingest("drain standard allocation without conversion", """
static int
__layered_copy_ingest_table(WT_SESSION_IMPL *session)
{
    WT_ERR(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, NULL));
    return (0);
}
""", 1, contains="without __wt_clayered_ingest_to_stable_value")

# A real tombstone allocation carries no user value and is exempt.
expect_ingest("tombstone allocation is exempt", """
static int
__layered_copy_ingest_table(WT_SESSION_IMPL *session)
{
    WT_ERR(__wt_upd_alloc_tombstone(session, &upd, NULL));
    return (0);
}
""", 0)

# An unconverted drain allocation wrapped across lines is still caught.
expect_ingest("drain allocation wrapped across lines", """
static int
__layered_copy_ingest_table(WT_SESSION_IMPL *session)
{
    WT_ERR(__wt_upd_alloc(
      session, value, WT_UPDATE_STANDARD, &upd, NULL));
    return (0);
}
""", 1, contains="without __wt_clayered_ingest_to_stable_value")

# Golden regression: the real ingest drain file must be clean, guarding against false positives.
real_ingest = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ste.INGEST_TARGET))
if os.path.exists(real_ingest):
    with open(real_ingest) as f:
        ingest_problems = ste.check_ingest_text(f.read())
    if ingest_problems:
        FAILURES.append(("real conn_layered_ingest.c is clean", 0, None, ingest_problems))
else:
    FAILURES.append(
        ("real conn_layered_ingest.c is present", 0, None, [f"missing {real_ingest}"]))

# Restore (prepared_discover_txn.c): a standard value converted to the ingest form before the
# allocation is correct.
expect_prepared("restore converts before the standard allocation", """
static int
__prepare_discover_alloc_upd(WT_SESSION_IMPL *session)
{
    WT_ERR(__wt_clayered_stable_to_ingest_value(session, value, &ingest_value, &tmp));
    WT_ERR(__wt_upd_alloc(session, &ingest_value, WT_UPDATE_STANDARD, &upd, sizep));
    return (0);
}
""", 0)

# A standard value restored with no conversion lands raw in the ingest table.
expect_prepared("restore standard allocation without conversion", """
static int
__prepare_discover_alloc_upd(WT_SESSION_IMPL *session)
{
    WT_ERR(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, sizep));
    return (0);
}
""", 1, contains="without __wt_clayered_stable_to_ingest_value")

# An allocation wrapped across lines is still matched.
expect_prepared("restore allocation wrapped across lines", """
static int
__prepare_discover_alloc_upd(WT_SESSION_IMPL *session)
{
    WT_ERR(__wt_upd_alloc(
      session, value, WT_UPDATE_STANDARD, &upd, sizep));
    return (0);
}
""", 1, contains="without __wt_clayered_stable_to_ingest_value")

# The stop-prepare delete artifact passes the marker itself and is exempt.
expect_prepared("restore of the delete artifact is exempt", """
static int
__prepare_discover_alloc_upd(WT_SESSION_IMPL *session)
{
    WT_ERR(__wt_upd_alloc(session, &__wt_tombstone, WT_UPDATE_STANDARD, &upd, sizep));
    return (0);
}
""", 0)

# Golden regression: the real prepared-discovery restore file must be clean.
real_prepared = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ste.PREPARED_TARGET))
if os.path.exists(real_prepared):
    with open(real_prepared) as f:
        prepared_problems = ste.check_prepared_text(f.read())
    if prepared_problems:
        FAILURES.append(("real prepared_discover_txn.c is clean", 0, None, prepared_problems))
else:
    FAILURES.append(
        ("real prepared_discover_txn.c is present", 0, None, [f"missing {real_prepared}"]))

# A missing anchor is reported loudly: text without the drain allocation must not pass silently.
anchor_problems = ste.check_anchors("""
static int
__layered_copy_ingest_table(WT_SESSION_IMPL *session)
{
    return (0);
}
""", ste.INGEST_TARGET)
if len(anchor_problems) != 1 or "silently disabled" not in anchor_problems[0]:
    FAILURES.append(("missing anchor fails loudly", 1, "silently disabled", anchor_problems))

# An anchor named only in a comment does not satisfy the check.
anchor_problems = ste.check_anchors("""
/*
 * The drain calls __wt_upd_alloc(session, ...) somewhere else these days.
 */
static int
__layered_copy_ingest_table(WT_SESSION_IMPL *session)
{
    return (0);
}
""", ste.INGEST_TARGET)
if len(anchor_problems) != 1:
    FAILURES.append(("anchor in a comment does not count", 1, None, anchor_problems))

# Golden regression: every real target must still carry its anchors.
for target in ste.ANCHORS:
    real_target = os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", target))
    if os.path.exists(real_target):
        with open(real_target) as f:
            anchor_problems = ste.check_anchors(f.read(), target)
        if anchor_problems:
            FAILURES.append((f"real {target} carries its anchors", 0, None, anchor_problems))
    else:
        FAILURES.append((f"real {target} is present", 0, None, [f"missing {real_target}"]))

# The callgraph rules: the output contract is locked down against canned tool output fed through
# an injected runner, then the real tool is run as a golden regression when the inspected files
# changed (mirroring the lint's own fast-mode gate).

# Parse contract: standalone anchor lines carry a file annotation, chains are callee first, the
# arrow grows dashes for cross-file and cross-directory calls, blank lines are ignored.
cg_anchors, cg_chains = ste.parse_callgraph("""
__clayered_deleted_encode      (src/cursor/cur_layered.c)

__clayered_deleted_encode <- __clayered_insert
__wt_clayered_ingest_to_stable_value <--- __layered_copy_ingest_table
__clayered_deleted_encode <- __clayered_insert <--- cursor->insert <--- __txn_op_apply
""")
if cg_anchors != {"__clayered_deleted_encode"} or cg_chains != [
        ["__clayered_deleted_encode", "__clayered_insert"],
        ["__wt_clayered_ingest_to_stable_value", "__layered_copy_ingest_table"],
        ["__clayered_deleted_encode", "__clayered_insert", "cursor->insert", "__txn_op_apply"]]:
    FAILURES.append(("callgraph output parses", 0, None, [cg_anchors, cg_chains]))


def canned_callgraph(drop_anchor=None, drop_lines=(), extra=()):
    # A synthetic D1/D2 walk output: one standalone line per anchor, the golden caller edges, and
    # the one required path that is longer than a single call.
    _, anchors = ste.callgraph_args()
    lines = [f"{a}      (src/x.c)" for a in anchors if a != drop_anchor]
    for target, callers in sorted(ste.CALLGRAPH_GOLDEN_CALLERS.items()):
        lines += [f"{target} <- {c}" for c in sorted(callers)]
    lines.append("__clayered_deleted_encode <- __clayered_modify_ingest <- __clayered_modify_int"
                 " <- __clayered_modify")
    return "\n".join([l for l in lines if l not in drop_lines] + list(extra)) + "\n"


# The body-anchored functions, kept consistent with the canned D1/D2 walk above so every default
# promoter and storer reaches its helper through the golden edges.
PROMOTERS = sorted(
    ste.CALLGRAPH_GOLDEN_CALLERS[ste.DECODE_CURRENT_FN] | {"__clayered_insert"})
STORERS = sorted(
    {"__clayered_modify_ingest", "__clayered_modify_stable"} | set(ste.CALLGRAPH_ENCODE_EXEMPT))


def canned_tagline(names, line):
    return "\n".join(f"{n} {line}      ()" for n in names) + "\n"


def fake_runner(main_out, promote_out=None, store_out=None):
    # Dispatch like the tool: the body scans carry --tagline, the D1/D2 walk does not.
    def run(args):
        if "--tagline" not in args:
            return main_out
        if args[1] == "///" + ste.CALLGRAPH_PROMOTE_BODY:
            if promote_out is not None:
                return promote_out
            return canned_tagline(PROMOTERS, "WT_ITEM_SET(cursor->value, current->value);")
        if store_out is not None:
            return store_out
        return canned_tagline(STORERS, "WT_ERR(c_ingest->update(c_ingest));")
    return run


def expect_callgraph(name, count, contains=None, **outputs):
    problems = ste.check_callgraph(fake_runner(**outputs))
    ok = len(problems) == count
    if contains is not None:
        ok = ok and any(contains in p for p in problems)
    if not ok:
        FAILURES.append((name, count, contains, problems))


# A complete graph produces no findings.
expect_callgraph("callgraph clean graph", 0, main_out=canned_callgraph())

# A missing anchor fails loudly, naming the function, and suppresses the follow-on noise.
expect_callgraph("callgraph missing anchor", 1,
    contains="__clayered_put() was not found",
    main_out=canned_callgraph(drop_anchor="__clayered_put"))

# Severing the only path from the modify entry point to the encode helper is caught.
expect_callgraph("callgraph severed reachability", 1,
    contains="__clayered_modify() no longer reaches __clayered_deleted_encode()",
    main_out=canned_callgraph(drop_lines=(
        "__clayered_deleted_encode <- __clayered_modify_ingest <- __clayered_modify_int"
        " <- __clayered_modify",)))

# A caller absent from the golden inventory is reported for review.
expect_callgraph("callgraph new caller", 1,
    contains="__clayered_rogue_writer() is a new direct caller",
    main_out=canned_callgraph(extra=("__clayered_deleted_encode <- __clayered_rogue_writer",)))

# A vanished caller is reported, alongside the reachability break it causes.
expect_callgraph("callgraph vanished caller", 2,
    contains="__clayered_update() no longer calls __clayered_deleted_encode() directly",
    main_out=canned_callgraph(drop_lines=("__clayered_deleted_encode <- __clayered_update",)))

# The tagline contract: matching body lines are joined onto the function's standalone line ahead
# of an empty parenthesized annotation.
cg_sites = ste.parse_callgraph_tagline(
    "__clayered_modify_ingest WT_ERR(c_ingest->update(c_ingest));"
    " WT_ERR(c_ingest->update(c_ingest));      ()\n\n"
    "__clayered_put c->set_value(c, value);      ()\n")
if cg_sites != {
        "__clayered_modify_ingest":
        "WT_ERR(c_ingest->update(c_ingest)); WT_ERR(c_ingest->update(c_ingest));",
        "__clayered_put": "c->set_value(c, value);"}:
    FAILURES.append(("callgraph tagline output parses", 0, None, [cg_sites]))

# Rule D3: a function that promotes a constituent value without reaching a decode is caught, and
# the finding quotes the offending line.
expect_callgraph("callgraph rogue promoter", 1,
    contains="__clayered_peek() promotes a constituent value"
             " [WT_ITEM_SET(cursor->value, current->value);]",
    promote_out=canned_tagline(
        PROMOTERS + ["__clayered_peek"], "WT_ITEM_SET(cursor->value, current->value);"),
    main_out=canned_callgraph())

# Rule D4: a function that stores into a constituent without reaching the encode helper is
# caught, unless exempted.
expect_callgraph("callgraph rogue storer", 1,
    contains="__clayered_bulk_store() stores a value into a constituent",
    store_out=canned_tagline(
        STORERS + ["__clayered_bulk_store"], "WT_ERR(c_ingest->update(c_ingest));"),
    main_out=canned_callgraph())

# A body idiom that matches nothing means the regex went stale; that is loud, not a clean pass.
expect_callgraph("callgraph stale body idiom", 1,
    contains="matches no function at all",
    promote_out="", main_out=canned_callgraph())

# An exemption whose function no longer matches the idiom is reported so the list stays fresh.
expect_callgraph("callgraph stale exemption", 1,
    contains="__clayered_remove_from_stable() matches no D4 site",
    store_out=canned_tagline(
        [s for s in STORERS if s != "__clayered_remove_from_stable"],
        "WT_ERR(c_ingest->update(c_ingest));"),
    main_out=canned_callgraph())

# A tool failure is a finding, not a silent pass.
def broken_runner(args):
    raise RuntimeError("perl not found")
cg_problems = ste.check_callgraph(broken_runner)
if len(cg_problems) != 1 or "failed: perl not found" not in cg_problems[0]:
    FAILURES.append(("callgraph tool failure fails loudly", 1, "failed", cg_problems))

# A failure in the body scans alone is reported per rule.
def broken_body_runner(args):
    if "--tagline" in args:
        raise RuntimeError("perl exploded")
    return canned_callgraph()
cg_problems = ste.check_callgraph(broken_body_runner)
if len(cg_problems) != 2 or any("failed: perl exploded" not in p for p in cg_problems):
    FAILURES.append(("callgraph body scan failure fails loudly", 2, "failed", cg_problems))

# Golden regression: the real call graph must be clean, proving the canned contract above still
# matches the tool's actual output.
if ste.callgraph_targets_changed():
    cg_problems = ste.check_callgraph(ste.run_callgraph)
    if cg_problems:
        FAILURES.append(("real call graph is clean", 0, None, cg_problems))

if FAILURES:
    for name, count, contains, problems in FAILURES:
        want = f"{count} finding(s)"
        if contains is not None:
            want += f" containing '{contains}'"
        print(f"test_tombstone_encoding: FAILED '{name}': expected {want}, got {problems}")
    sys.exit(1)
