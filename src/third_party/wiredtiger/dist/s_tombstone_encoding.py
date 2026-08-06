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

# Check that the layered cursor keeps tombstone value encoding wired up correctly.
#
# Layered tables escape user values that begin with the reserved ingest tombstone marker (see
# the "Tombstone value encoding" comment in src/cursor/cur_layered.c). Four invariants keep the
# escaping consistent, and all are easy to break silently because the escaped values are rare:
#
#   1. Every value promoted from a constituent cursor up to the user-facing layered cursor must be
#      run through __clayered_deleted_decode() (or the __clayered_decode_current() wrapper) before
#      it is exposed. A read path that copies current_cursor->value into cursor->value and returns
#      without decoding hands the caller the escaped bytes.
#
#   2. Every __clayered_deleted_encode()/__clayered_deleted_decode() call must take its full
#      argument list, carrying a constituent-derived decision (does this value belong to, or come
#      from, the stable table?) as its third argument. A call that drops the argument or passes an
#      unrelated value escapes or strips against the wrong table.
#
#   3. Every value drained from the ingest table into the stable table (in
#      src/conn/conn_layered_ingest.c) must be converted to the stable form before it is stored, so
#      an unescaped stable on-disk image never inherits the escape byte.
#
#   4. Every prepared value restored from a stable checkpoint cell into the ingest table (in
#      src/prepared_discover/prepared_discover_txn.c) must be converted to the ingest form first,
#      so a raw marker-prefixed value from an unescaped stable image regains its escape byte.
#
# This is a structural lint, not a proof: it flags promotions with no nearby decode, malformed
# encode/decode calls, and drain or restore allocations that skip the required conversion. Sites
# that are genuinely allowed to skip a decode are listed, with a ticket, in ALLOWED_MISSING_DECODE
# below rather than being silently ignored. A path that never calls a conversion helper at all
# leaves nothing for the text rules to match, so the callgraph rules (D1-D4 below) cover that gap
# at call-graph granularity: the named write and cross-file paths must still reach their
# conversion helper (D1), the helpers' direct-caller sets must match a reviewed inventory (D2),
# and any function whose body promotes or stores a constituent value -- named anywhere or not --
# must reach a decode or the encode helper (D3/D4).
#
# This is a static dist check rather than a runtime test because it verifies source wiring, not
# behavior: a violation corrupts silently and only for rare marker-prefixed values, so the python
# suites cannot reliably reach every cursor path with a marker-prefixed value. The suites cover the
# behavior; this covers the wiring, at edit time via s_fast.
#
# Maintenance. The ingest-table escaping is permanent, so these checks outlive the stable-table
# legacy support: removing that support only simplifies them (the constituent decisions and the
# stable-side gating go; decode-on-read, the drain and restore conversions, and the marker
# confinement stay). When the lint fires on a site that is genuinely correct, record the exception
# in the matching allowlist with a reason (ALLOWED_MISSING_DECODE, CALLGRAPH_ENCODE_EXEMPT, the
# golden caller inventory) rather than loosening a pattern: the patterns are shared, an exception
# is one site. When a correct new idiom is not matched at all (a false negative), extend the idiom
# regex and add a fixture to test_tombstone_encoding.py proving the new form is caught; the
# self-test is the specification, and it must be run after any change here.
#
# FIXME-WT-18244: once value transfers are routed through encode/decode wrappers, most of these
# rules police sites that can no longer exist. Reconsider what this check still buys and remove it
# (with test_tombstone_encoding.py) unless enough justification remains.

import os
import re
import subprocess
import sys

from common_functions import filter_if_fast

# The layered cursor read/write paths. The prefix must match how filter_if_fast sees changed file
# names (see its docstring).
TARGET = "src/cursor/cur_layered.c"
PREFIX = "../"

# The ingest->stable drain lives in a second file. Values copied from the ingest table (always
# escaped) into the stable table must be converted to the stable form before they are stored, or an
# unescaped stable on-disk image inherits the escape byte. The single drain path that carries a
# value is the standard-value allocation.
INGEST_TARGET = "src/conn/conn_layered_ingest.c"

# A standard-value update allocated on the drain path. Real tombstones use __wt_upd_alloc_tombstone
# and carry no user value, so they are not matched.
UPD_ALLOC_STANDARD_RE = re.compile(r"__wt_upd_alloc\([^;]*WT_UPDATE_STANDARD")
STABLE_VALUE_STRIP = "__wt_clayered_ingest_to_stable_value("

# Prepared discovery on a follower restores prepared updates from the stable checkpoint into the
# ingest table. A restored standard value must be converted to the ingest form first; the
# stop-prepare delete artifact is the marker itself and is exempt.
PREPARED_TARGET = "src/prepared_discover/prepared_discover_txn.c"
STABLE_TO_INGEST = "__wt_clayered_stable_to_ingest_value("
UPD_ALLOC_CALL = "__wt_upd_alloc("
TOMBSTONE_VALUE_ARG = "&__wt_tombstone"

# A value belonging to one of the constituent cursors. These are the sources that carry encoded
# bytes and so must be decoded before reaching the user, and the destinations that must be encoded.
CONSTITUENT = (
    r"clayered->current_cursor|current|op\.stable|op\.ingest|"
    r"clayered->stable_cursor|clayered->ingest_cursor|c_stable|c_ingest"
)

# A promotion of a constituent value into the user-facing layered cursor (cursor/iface is always
# &clayered->iface in this file).
PROMOTE_RE = re.compile(
    r"WT_ITEM_SET\((?:cursor|iface)->value\s*,\s*(?:" + CONSTITUENT + r")->value\)")

# The dominant read idiom never touches WT_ITEM_SET: a lookup, or a constituent get_value, writes
# straight into the user cursor's value and the caller decodes afterwards. The destination is pinned
# to the user-facing value so the write paths, which look a value up into a local WT_ITEM before
# re-encoding it, are not mistaken for reads that still owe a decode.
LOOKUP_PROMOTE_RE = re.compile(
    r"(?:__clayered_lookup|get_value)\([^;]*&(?:cursor|iface)->value\s*\)")

DECODE_CALL = "__clayered_deleted_decode("
ENCODE_CALL = "__clayered_deleted_encode("

# The thin wrapper that decodes the layered cursor's current constituent; it satisfies the Rule 1
# decode obligation just as a direct __clayered_deleted_decode() call does.
DECODE_WRAPPER = "__clayered_decode_current("

# The 0-based index of the constituent-decision argument in an encode/decode call: for encode
# (session, value, to_stable, final_value, tmpp) and decode (session, value, from_stable) alike it
# is the third argument.
DECISION_ARG = 2

# Tokens that make the constituent-decision argument legitimate. A bare boolean literal is allowed
# because the modify helpers know their target table statically.
ARG_OK = re.compile(r"stable_cursor|current_cursor|ingest|^\s*(?:true|false)\b")

# The reserved marker itself: the __wt_tombstone global, or a literal 0x14 / \x14 byte. Escaping and
# stripping must stay inside the sanctioned helpers so the stable-encoding switch remains the only
# place deciding whether stable values participate; a hand-rolled marker test or byte append
# anywhere else would silently bypass it.
RAW_MARKER_RE = re.compile(r"__wt_tombstone|(?:0x14|\\x14)(?![0-9a-fA-F])")

# Recording a delete writes the real marker into a constituent; that is not a hand-rolled escape.
TOMBSTONE_WRITE_RE = re.compile(r"set_value\([^;]*&__wt_tombstone")

# The only functions allowed to handle the raw marker: the shared namespace test, the encode/decode
# helpers, and the stat/drain helpers that classify the escaped stable form.
TOMBSTONE_ALLOWED = {
    "__clayered_value_in_tombstone_namespace",
    "__clayered_deleted_encode",
    "__clayered_deleted_decode",
    "__wt_clayered_stable_value_stat",
    "__wt_clayered_ingest_to_stable_value",
}

# Functions whose promotion is knowingly not decoded, mapped to the ticket documenting why. Keyed by
# the enclosing function so the check still fires on any *new* promotion added elsewhere in the same
# function; when a skip applies the ticket is printed rather than the site being silently dropped.
# Empty: every read path decodes.
ALLOWED_MISSING_DECODE = {}

# The named functions each target's rules are keyed to. A rename or removal would make the rule
# that matches on the name silently stop firing, so the anchors are verified against the target's
# code (comments stripped) and an absence is reported as a problem in its own right: the helpers
# and their definitions in the layered cursor, and the update allocation the drain and restore
# rules trigger on.
ANCHORS = {
    TARGET: (DECODE_CALL, ENCODE_CALL, STABLE_VALUE_STRIP, STABLE_TO_INGEST),
    INGEST_TARGET: (UPD_ALLOC_CALL,),
    PREPARED_TARGET: (UPD_ALLOC_CALL,),
}


def check_anchors(text, target):
    code = strip_block_comments(text)
    problems = []
    for anchor in ANCHORS.get(target, ()):
        if anchor not in code:
            problems.append(
                f"{target}: anchor {anchor}) is gone; a rule keyed to it is silently disabled, "
                f"update {os.path.basename(__file__)}")
    return problems


# The conversion helpers as call-graph nodes rather than call-site tokens.
ENCODE_FN = ENCODE_CALL[:-1]
DECODE_FN = DECODE_CALL[:-1]
DECODE_CURRENT_FN = DECODE_WRAPPER[:-1]
INGEST_TO_STABLE_FN = STABLE_VALUE_STRIP[:-1]
STABLE_TO_INGEST_FN = STABLE_TO_INGEST[:-1]

# The static call-graph analyzer the D rules run, relative to the repository root.
CALLGRAPH_TOOL = "tools/callgraph"

# Maximum chain length (in functions) for a reachability path. The deepest required path today is
# modify -> modify_int -> modify_{ingest,stable} -> encode, four functions.
CALLGRAPH_DEPTH = 5

# Rule D1: each named entry point must reach one of its conversion helpers, as (entry point,
# acceptable helpers). Only the write paths and the two cross-file conversions are named: their
# obligation holds whatever their bodies look like, so the name anchor survives idiom drift. The
# read paths are not named here because their obligation exists only when they promote a value,
# which rule D3 anchors on the promotion idiom itself.
CALLGRAPH_REACHABILITY = (
    ("__clayered_insert", (ENCODE_FN,)),
    ("__clayered_update", (ENCODE_FN,)),
    ("__clayered_modify", (ENCODE_FN,)),
    ("__layered_copy_ingest_table", (INGEST_TO_STABLE_FN,)),
    ("__prepare_discover_alloc_upd", (STABLE_TO_INGEST_FN,)),
)

# Rule D2: the reviewed direct callers of each conversion helper, plus the one store helper that
# writes caller-encoded bytes. A caller appearing here means its use of the helper was checked
# against the invariants above; a new or vanished caller fails until the site is reviewed and
# this inventory updated.
CALLGRAPH_GOLDEN_CALLERS = {
    ENCODE_FN: frozenset({
        "__clayered_insert", "__clayered_modify_ingest", "__clayered_modify_stable",
        "__clayered_update", STABLE_TO_INGEST_FN}),
    DECODE_FN: frozenset({
        "__clayered_decode_current", "__clayered_insert", "__clayered_modify_stable",
        "__clayered_modify_try_ingest", INGEST_TO_STABLE_FN}),
    DECODE_CURRENT_FN: frozenset({
        "__clayered_copy_duplicate_kv", "__clayered_iterate", "__clayered_modify",
        "__clayered_modify_ingest", "__clayered_next_random", "__clayered_search",
        "__clayered_search_near", "__clayered_update"}),
    INGEST_TO_STABLE_FN: frozenset({"__layered_copy_ingest_table"}),
    STABLE_TO_INGEST_FN: frozenset({"__prepare_discover_alloc_upd"}),
    # Not a conversion helper: it stores bytes its callers already encoded, so it is exempt from
    # rule D4 and its caller set is pinned here instead.
    "__clayered_put": frozenset({
        "__clayered_insert", "__clayered_reserve", "__clayered_update"}),
}

# Rules D3/D4 anchor on behavior rather than names, using the tool's ///content-regex form: every
# function whose body matches the idiom is obligated, so a brand-new path is caught without being
# listed anywhere. The idioms are constituent-scoped (same sources as CONSTITUENT above) so they
# do not anchor at unrelated cursor code elsewhere in the tree.

# Rule D3: a body that promotes a constituent value to the user cursor must reach a decode.
CALLGRAPH_PROMOTE_BODY = (
    r"WT_ITEM_SET\((cursor|iface)->value, (" + CONSTITUENT + r")->value\)"
    r"|(__clayered_lookup|get_value)\([^;]*&(cursor|iface)->value\)")

# Rule D4: a body that stores a value into a constituent must reach the encode helper. The bare
# "c" form is the store helper's own set_value.
CALLGRAPH_STORE_BODY = (
    r"(c_stable|c_ingest|op\.stable|op\.ingest|clayered->stable_cursor|clayered->ingest_cursor)"
    r"->(set_value|insert|update|remove)\(|c->set_value\(c, value\)")

# Body-anchored functions allowed to skip the conversion, mapped to the reason. An entry that no
# longer matches any site is reported so the lists cannot go stale.
CALLGRAPH_DECODE_EXEMPT = {}
CALLGRAPH_ENCODE_EXEMPT = {
    "__clayered_put": "stores bytes its callers already encoded; the callers are pinned in "
                      "CALLGRAPH_GOLDEN_CALLERS",
    "__clayered_remove_from_ingest": "records a delete by storing the raw tombstone marker",
    "__clayered_remove_from_stable": "removes the stable row and stores no value",
}

# In reverse mode the tool prints one standalone line per resolved -t anchor and one line per
# caller path, callee first: "callee <- caller <- ...". The arrow grows dashes for cross-file and
# cross-directory calls, so match any length.
CALLGRAPH_ARROW_RE = re.compile(r"\s<-+\s")


def callgraph_args():
    # Rules D1/D2 are served by one batched reverse walk from every named anchor: standalone
    # lines prove the anchors still exist, chains headed by a helper carry its callers and
    # ancestors.
    anchors = sorted(set(CALLGRAPH_GOLDEN_CALLERS) | {s for s, _ in CALLGRAPH_REACHABILITY})
    args = []
    for name in anchors:
        args += ["-t", name]
    return args + ["-d", str(CALLGRAPH_DEPTH), "-r", "-no-l"], anchors


def callgraph_body_args(body_re):
    # One body-anchored scan per idiom: -t ///RE resolves every function whose body matches, and
    # the matching --tagline quotes the offending lines on each function's standalone line.
    return ["-t", "///" + body_re, "--tagline", "/" + body_re, "-d", "1", "-r", "-no-l"]


def run_callgraph(args):
    # Run from the repository root so the tool scans the same tree whether the lint is started
    # from dist/ (as s_all does) or from the root.
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    proc = subprocess.run(
        [os.path.join(root, CALLGRAPH_TOOL), *args], capture_output=True, text=True, cwd=root)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"exit status {proc.returncode}")
    return proc.stdout


def parse_callgraph(output):
    # Split the tool's stdout into the resolved anchors and the caller chains (callee first).
    # Progress and stats go to stderr, so every stdout line is an anchor or a chain.
    anchors = set()
    chains = []
    for line in output.splitlines():
        names = [seg.split()[0] for seg in CALLGRAPH_ARROW_RE.split(line.strip()) if seg.strip()]
        if len(names) == 1:
            anchors.add(names[0])
        elif names:
            chains.append(names)
    return anchors, chains


# A --tagline standalone line: the function name, the matching body lines joined by single
# spaces, and an empty parenthesized annotation.
CALLGRAPH_TAGLINE_RE = re.compile(r"\s+\(\)$")


def parse_callgraph_tagline(output):
    # Map each body-matched function to the quoted text of its matching lines.
    sites = {}
    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        name, _, quoted = line.partition(" ")
        sites[name] = CALLGRAPH_TAGLINE_RE.sub("", quoted).strip()
    return sites


def check_callgraph_body(run, ancestors):
    # Rules D3/D4: every function whose body matches the idiom must reach one of the helpers,
    # unless exempted for a stated reason. The ancestor sets come from the D1/D2 walk.
    problems = []
    me = os.path.basename(__file__)
    rules = (
        ("D3", CALLGRAPH_PROMOTE_BODY, (DECODE_FN, DECODE_CURRENT_FN), CALLGRAPH_DECODE_EXEMPT,
         "CALLGRAPH_DECODE_EXEMPT", "promotes a constituent value",
         "the caller may see escaped tombstone bytes"),
        ("D4", CALLGRAPH_STORE_BODY, (ENCODE_FN,), CALLGRAPH_ENCODE_EXEMPT,
         "CALLGRAPH_ENCODE_EXEMPT", "stores a value into a constituent",
         "a raw marker-prefixed user value can reach the table"),
    )
    for rule, body_re, targets, exempt, exempt_name, does, consequence in rules:
        args = callgraph_body_args(body_re)
        scan = f"{CALLGRAPH_TOOL} -t '///{body_re}' --tagline '/{body_re}' -d 1 -r -no-l"
        try:
            output = run(args)
        except (OSError, RuntimeError) as e:
            problems.append(f"callgraph: {CALLGRAPH_TOOL} {' '.join(args)} failed: {e}")
            continue
        sites = parse_callgraph_tagline(output)
        # An idiom that matches nothing means the regex went stale and the rule is disabled.
        if not sites:
            problems.append(
                f"callgraph: the rule {rule} body idiom matches no function at all; the idiom "
                f"regex is stale and the rule is silently disabled, update {me} "
                f"(reproduce: {scan})")
            continue
        for name in sorted(sites):
            if any(name in ancestors.get(t, ()) for t in targets):
                continue
            if name in exempt:
                continue
            alt = " or ".join(t + "()" for t in targets)
            repro = (f"{CALLGRAPH_TOOL} -f {name} " + " ".join("-t " + t for t in targets) +
                     f" -d {CALLGRAPH_DEPTH} -no-l")
            problems.append(
                f"callgraph: {name}() {does} [{sites[name]}] but does not reach {alt} within "
                f"{CALLGRAPH_DEPTH} calls; {consequence}, add the conversion or exempt it with "
                f"a reason in {exempt_name} (reproduce: {repro})")
        for name in sorted(set(exempt) - set(sites)):
            problems.append(
                f"callgraph: the {exempt_name} entry for {name}() matches no {rule} site; "
                f"remove it or update the idiom in {me} (reproduce: {scan})")
    return problems


def check_callgraph(run):
    args, anchors = callgraph_args()
    me = os.path.basename(__file__)
    try:
        output = run(args)
    except (OSError, RuntimeError) as e:
        return [f"callgraph: {CALLGRAPH_TOOL} {' '.join(args)} failed: {e}"]
    found, chains = parse_callgraph(output)
    # A -t that matches no function is dropped from an otherwise successful multi-anchor run, so
    # a renamed anchor must be caught here: every resolved anchor prints one standalone line. Do
    # not go on to the D rules, which would only repeat the failure as bogus unreachability.
    missing = [a for a in anchors if a not in found]
    if missing:
        return [
            f"callgraph: anchor function {name}() was not found; the rules keyed to it are "
            f"silently disabled, update {me} (reproduce: {CALLGRAPH_TOOL} -t {name} -d 1 -r)"
            for name in missing]
    problems = []
    ancestors = {}
    callers = {}
    for chain in chains:
        ancestors.setdefault(chain[0], set()).update(chain[1:])
        callers.setdefault(chain[0], set()).add(chain[1])
    # Rule D1: every entry point still reaches a conversion helper.
    for source, targets in CALLGRAPH_REACHABILITY:
        if any(source in ancestors.get(t, ()) for t in targets):
            continue
        alt = " or ".join(t + "()" for t in targets)
        repro = (f"{CALLGRAPH_TOOL} -f {source} " + " ".join("-t " + t for t in targets) +
                 f" -d {CALLGRAPH_DEPTH} -no-l")
        problems.append(
            f"callgraph: {source}() no longer reaches {alt} within {CALLGRAPH_DEPTH} calls; a "
            f"layered path with no conversion ships raw or escaped tombstone bytes "
            f"(reproduce: {repro})")
    # Rule D2: the direct callers of each conversion helper match the reviewed inventory.
    for target, expected in sorted(CALLGRAPH_GOLDEN_CALLERS.items()):
        actual = callers.get(target, set())
        repro = f"{CALLGRAPH_TOOL} -t {target} -d 2 -r -no-l"
        for caller in sorted(actual - expected):
            problems.append(
                f"callgraph: {caller}() is a new direct caller of {target}(); review the site "
                f"against the encoding invariants, then add it to CALLGRAPH_GOLDEN_CALLERS in "
                f"{me} (reproduce: {repro})")
        for caller in sorted(expected - actual):
            problems.append(
                f"callgraph: {caller}() no longer calls {target}() directly; if the conversion "
                f"moved intentionally, update CALLGRAPH_GOLDEN_CALLERS in {me} "
                f"(reproduce: {repro})")
    return problems + check_callgraph_body(run, ancestors)


def callgraph_targets_changed():
    # The D rules span every inspected file, so in fast mode they run when any of them changed.
    return any(
        list(filter_if_fast(iter([PREFIX + target]), prefix=PREFIX)) for target in ANCHORS)


def split_functions(lines):
    # WiredTiger style puts a function body's opening and closing brace in column zero, and inner
    # blocks are indented, so a standalone "{" / "}" pair delimits one function body. Yield
    # (name, start_line_index, end_line_index) spanning the body of each function.
    funcs = []
    name = None
    start = None
    for i, line in enumerate(lines):
        if line == "{" and start is None:
            # The signature is the non-empty line above the brace; pull the identifier from it.
            for j in range(i - 1, -1, -1):
                m = re.search(r"(\w+)\s*\(", lines[j])
                if m:
                    name = m.group(1)
                    break
            start = i
        elif line == "}" and start is not None:
            funcs.append((name, start, i))
            name = start = None
    return funcs


def call_arg_count(text, open_paren):
    # Given text and the index of the "(" opening a call, return the number of top-level comma
    # separated arguments and the list of their raw texts, or (None, None) if the call is not
    # closed within the text.
    depth = 0
    args = []
    cur = []
    i = open_paren
    while i < len(text):
        c = text[i]
        if c == "(":
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(cur))
                return len(args), args
        if depth == 1 and c == ",":
            args.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    return None, None


def strip_block_comments(text):
    # Replace every /* ... */ block, including multi-line ones, with as many newlines as it spanned.
    # Line numbers are preserved so findings still point at real source lines, and a marker or a
    # "decode" mention that lives only in prose (the encoding overview, a function header) cannot
    # trip the scanners.
    def repl(m):
        return "\n" * m.group(0).count("\n")

    return re.sub(r"/\*.*?\*/", repl, text, flags=re.DOTALL)


def check_text(text, skips=None):
    problems = []
    code = strip_block_comments(text).split("\n")
    funcs = split_functions(code)

    def enclosing(idx):
        for name, s, e in funcs:
            if s <= idx <= e:
                return name, s, e
        return None, idx, idx

    # Rule 1: a constituent value surfaced on the user cursor -- copied with WT_ITEM_SET, or
    # looked up straight into cursor->value -- must be followed by a decode in the same function.
    for i, ln in enumerate(code):
        promote = PROMOTE_RE.search(ln)
        lookup = LOOKUP_PROMOTE_RE.search(ln)
        if not promote and not lookup:
            continue
        name, _, end = enclosing(i)
        if any(
          DECODE_CALL in code[j] or DECODE_WRAPPER in code[j] for j in range(i, end + 1)):
            continue
        if name in ALLOWED_MISSING_DECODE:
            if skips is not None:
                skips.append(
                    f"{TARGET}:{i + 1}: decode intentionally skipped in {name}() "
                    f"({ALLOWED_MISSING_DECODE[name]})")
            continue
        how = "promoted to" if promote else "looked up into"
        problems.append(
            f"{TARGET}:{i + 1}: value {how} the layered cursor in {name}() is not passed "
            f"through {DECODE_CALL}); the caller may see escaped tombstone bytes")

    # Rule 2: encode/decode calls must be well-formed and carry a constituent decision. Join each
    # call's (possibly wrapped) text so a call split across lines is still parsed.
    joined = "\n".join(code)
    for token, want in ((ENCODE_CALL, 5), (DECODE_CALL, 3)):
        for m in re.finditer(re.escape(token), joined):
            # Skip the function definition itself (column zero, i.e. preceded by a newline).
            if m.start() == 0 or joined[m.start() - 1] == "\n":
                continue
            open_paren = m.start() + len(token) - 1
            count, args = call_arg_count(joined, open_paren)
            lineno = joined.count("\n", 0, m.start()) + 1
            if count is None:
                continue
            if count != want:
                problems.append(
                    f"{TARGET}:{lineno}: {token}) called with {count} arguments, expected {want}")
            else:
                decision = args[DECISION_ARG]
                if not ARG_OK.search(decision.strip()):
                    problems.append(
                        f"{TARGET}:{lineno}: {token}) argument '{decision.strip()}' is not a "
                        f"constituent decision (expected a stable/ingest/current_cursor test or "
                        f"a true/false literal)")

    # Rule 3: the raw tombstone marker may only appear inside the sanctioned helpers. Anywhere else
    # it is a hand-rolled escape or strip that bypasses the stable-encoding switch; writing the real
    # marker to record a delete is exempt.
    for i, ln in enumerate(code):
        if not RAW_MARKER_RE.search(ln) or TOMBSTONE_WRITE_RE.search(ln):
            continue
        name, _, _ = enclosing(i)
        if name in TOMBSTONE_ALLOWED:
            continue
        problems.append(
            f"{TARGET}:{i + 1}: raw tombstone marker used in {name}() outside the sanctioned "
            f"encode/decode helpers; a hand-rolled escape bypasses the stable-encoding switch")

    return problems


def check_ingest_text(text):
    problems = []
    code = strip_block_comments(text).split("\n")
    funcs = split_functions(code)

    def enclosing(idx):
        for name, s, e in funcs:
            if s <= idx <= e:
                return name, s, e
        return None, idx, idx

    # Rule B1: a standard value drained into the stable table must be converted to the stable form
    # first; the conversion must appear before the allocation in the same function. Real tombstones
    # use __wt_upd_alloc_tombstone and carry no value, so they are not matched. Matches are taken
    # from the joined text so an allocation wrapped across lines is still caught.
    joined = "\n".join(code)
    for m in UPD_ALLOC_STANDARD_RE.finditer(joined):
        i = joined.count("\n", 0, m.start())
        name, start, _ = enclosing(i)
        if any(STABLE_VALUE_STRIP in code[j] for j in range(start, i + 1)):
            continue
        problems.append(
            f"{INGEST_TARGET}:{i + 1}: a standard value is drained to the stable table in {name}() "
            f"without {STABLE_VALUE_STRIP}); an unescaped stable image inherits the escape byte")

    return problems


def check_prepared_text(text):
    problems = []
    code = strip_block_comments(text).split("\n")
    funcs = split_functions(code)
    joined = "\n".join(code)

    def enclosing(idx):
        for name, s, e in funcs:
            if s <= idx <= e:
                return name, s, e
        return None, idx, idx

    # Rule C1: a standard value restored into the ingest table must be converted to the ingest form
    # first; the conversion must appear before the allocation in the same function. The stop-prepare
    # delete artifact passes the marker itself and is exempt. Calls are parsed from the joined text
    # so an allocation wrapped across lines is still matched.
    for m in re.finditer(re.escape(UPD_ALLOC_CALL), joined):
        count, args = call_arg_count(joined, m.start() + len(UPD_ALLOC_CALL) - 1)
        if count is None or count < 3 or "WT_UPDATE_STANDARD" not in args[2]:
            continue
        if args[1].strip() == TOMBSTONE_VALUE_ARG:
            continue
        i = joined.count("\n", 0, m.start())
        name, start, _ = enclosing(i)
        if any(STABLE_TO_INGEST in code[j] for j in range(start, i + 1)):
            continue
        problems.append(
            f"{PREPARED_TARGET}:{i + 1}: a prepared value is restored to the ingest table in "
            f"{name}() without {STABLE_TO_INGEST}); a raw stable value bypasses the ingest escape")

    return problems


def check(path):
    with open(path) as f:
        return check_text(f.read())


def main():
    # Resolve targets from this script's location so the check behaves the same whether it is run
    # from dist/ (as s_all does) or from the repository root. Three files are inspected: the
    # layered cursor read/write paths, the ingest->stable drain, and the prepared-discovery
    # restore.
    script_dir = os.path.dirname(os.path.abspath(__file__))
    problems = []
    skips = []
    checkers = {
        TARGET: lambda text: check_text(text, skips),
        INGEST_TARGET: check_ingest_text,
        PREPARED_TARGET: check_prepared_text,
    }
    for target, checker in checkers.items():
        path = os.path.normpath(os.path.join(script_dir, "..", target))
        # If a target has been renamed or moved, fail loudly rather than parse a missing path or
        # quietly pass; the constant must be updated to match.
        if not os.path.exists(path):
            print(f"{target} is missing or was renamed; update {os.path.basename(__file__)}")
            return 1
        # In fast mode only inspect a target that actually changed.
        if not list(filter_if_fast(iter([PREFIX + target]), prefix=PREFIX)):
            continue
        with open(path) as f:
            text = f.read()
        problems += check_anchors(text, target) + checker(text)
    if callgraph_targets_changed():
        problems += check_callgraph(run_callgraph)
    for note in skips:
        print(note)
    for p in sorted(problems):
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
