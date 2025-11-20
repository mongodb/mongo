#!/usr/bin/env python3
"""Markdown Link Linter (MongoDB)
=================================

Checks Markdown files under `src/mongo` for broken internal links.

Link Types Validated
--------------------
1. Intra-document anchors: `[text](#some-heading)`
2. Relative file links: `[text](../../path/to/OtherFile.md#anchor)`
3. Repo-root relative paths beginning with `/src/` (e.g. `[feature flags](/src/mongo/db/query/README_query_feature_flags.md)`).

External (http/https) links are currently skipped (no network requests performed) except for a trivial malformed scheme check (e.g. `hhttps://`).

GitHub Repository Link Policy
-----------------------------
Links to the MongoDB server repository (`github.com/mongodb/mongo` or private clone `github.com/10gen/mongo`) must not reference the mutable `master` branch. Allowed:
* Release/tag branches (e.g. `r6.2.0`)
* Specific commit SHAs (40 hex chars)
* Any other non-`master` branch

Unpinned `master` links are reported with an issue. Auto-fix rewrites them to a repo-root relative path (`/src/...`) preserving any line fragment (e.g. `#L89`).

Anchor Normalization
--------------------
GitHub-style anchors derived from headings:
* Lowercased
* Punctuation stripped (most symbols except `-` and `_`)
* Spaces collapsed to single `-`

Usage
-----
Run from repository root:
    python buildscripts/lint_markdown_links.py --verbose

JSON output (exit code still meaningful):
    python buildscripts/lint_markdown_links.py --json > link_report.json

Auto-Fix Renamed Paths
----------------------
Auto-fix (`--auto-fix`) automatically handles:
* Directory renames via `--rename-map old=new`
* Moved files (searches by basename across the repository)
* Broken anchors (relocates to correct file)
* Common typos and malformed schemes

Example with rename mapping:
    python buildscripts/lint_markdown_links.py --auto-fix --rename-map catalog=local_catalog --root src/mongo/db/storage --verbose

Multiple mappings:
    python buildscripts/lint_markdown_links.py --auto-fix \
        --rename-map catalog=local_catalog \
        --rename-map query_stats=query_shape_stats

After auto-fix the script re-runs linting to verify all fixes.

Safety Characteristics
----------------------
* Only replaces the specific `](oldpath...)` occurrence.
* Skips if replacement yields identical path.
* Always review diffs before committing.

Exit Codes
----------
0 = all links OK
1 = usage / root not found
2 = one or more link issues detected

Sample Output
-------------
    src/mongo/example/README.md:42: file does not exist: /abs/path/src/mongo/missing.md [missing.md]
    src/mongo/example/README.md:57: anchor "overview" not found in target file [other.md#overview]

Common False Positives
----------------------
* Headings generated dynamically (e.g. code-generated docs)
* Links to files produced by a build step
* Root-relative paths not starting with `/src/` (extend logic if needed)
* External links (intentionally not validated)

Performance
-----------
Parallel validation using a thread pool sized to available CPUs (capped at 32).

Suppressing Specific Links
--------------------------
Not implemented yet. Potential future directive:
    <!-- linklint-ignore-next -->

Future Enhancements (Ideas)
---------------------------
* Reference-style link resolution (`[text][ref]` definitions)
* Ignore patterns via CLI or config file
* CI integration (Bazel / GitHub Actions) enforcing link health
* Levenshtein suggestions for typos
* Anchor remapping when heading text changes

CI Integration Example
----------------------
Add a step:
    python buildscripts/lint_markdown_links.py --json
Fail build if exit code is 2.

Implementation Notes
--------------------
* Uses regex heuristics (no full Markdown parsing) for speed.
* Anchor generation and link fragment normalization share the same logic (`github_anchor`).

Maintained by MongoDB Engineering Tooling.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import sys
import urllib.parse
from dataclasses import dataclass
from typing import Iterable, List, Optional, Tuple

HEADING_RE = re.compile(r"^(#{1,6})\s+(.*)$")
HTML_ANCHOR_RE = re.compile(r'<a\s+(?:name|id)=["\']([^"\']+)["\']\s*>\s*</a>?', re.IGNORECASE)
LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
# Inline link references: [text]: url
REF_DEF_RE = re.compile(r"^\s*\[([^\]]+)\]:\s+(\S+)")
REF_USE_RE = re.compile(r"\[([^\]]+)\]\[(?:(?:[^\]]+))?\]")  # simplified

# Characters removed for anchor IDs (GitHub rules simplified). We strip most punctuation except hyphen and underscore.
PUNCT_TO_STRIP = "\"'!#$%&()*+,./:;<=>?@[]^`{|}~"  # punctuation characters to remove
ANCHOR_CACHE: dict[str, set[str]] = {}


def _detect_repo_root(start: str | None = None) -> str:
    """Walk upwards to locate repository root (presence of WORKSPACE.bazel or .git).

    Falls back to current working directory if no sentinel found.
    """
    if start is None:
        start = os.getcwd()
    cur = os.path.abspath(start)
    last = None
    while cur != last:
        if (
            os.path.exists(os.path.join(cur, "WORKSPACE.bazel"))
            or os.path.exists(os.path.join(cur, "MODULE.bazel"))
            or os.path.isdir(os.path.join(cur, ".git"))
        ):
            return cur
        last = cur
        cur = os.path.dirname(cur)
    return os.getcwd()


REPO_ROOT = _detect_repo_root()


@dataclass
class LinkIssue:
    file: str
    line: int
    link_text: str
    target: str
    message: str

    def to_dict(self):
        return {
            "file": self.file,
            "line": self.line,
            "link_text": self.link_text,
            "target": self.target,
            "message": self.message,
        }


def github_anchor(text: str) -> str:
    t = text.strip().lower()
    # remove punctuation
    t2 = "".join(ch for ch in t if ch not in PUNCT_TO_STRIP)
    # spaces to hyphens
    t2 = re.sub(r"\s+", "-", t2)
    # collapse multiple hyphens
    t2 = re.sub(r"-+", "-", t2)
    return t2


def collect_headings(path: str) -> set[str]:
    if path in ANCHOR_CACHE:
        return ANCHOR_CACHE[path]
    anchors: set[str] = set()
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                # Support blockquoted headings: strip leading '>' plus following space(s)
                if line.lstrip().startswith(">"):
                    # Remove successive '>' prefixes (nested blockquotes) while preserving heading markers
                    stripped = line.lstrip()
                    while stripped.startswith(">"):
                        stripped = stripped[1:].lstrip()
                    candidate_line = stripped
                else:
                    candidate_line = line
                m = HEADING_RE.match(candidate_line)
                if m:
                    heading_text = m.group(2)
                    # Extract any embedded HTML anchors first
                    for a in HTML_ANCHOR_RE.finditer(heading_text):
                        raw = a.group(1).strip()
                        if raw:
                            anchors.add(github_anchor(raw))
                            anchors.add(raw)  # also allow direct reference without normalization
                    # Remove HTML anchor tags from heading text before normalizing (support no-space join)
                    cleaned = HTML_ANCHOR_RE.sub("", heading_text).strip()
                    # Strip inline markdown links (e.g., Classic [PlanCache](...)) to derive anchor from visible text only.
                    cleaned = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", cleaned)
                    if cleaned:
                        norm_clean = github_anchor(cleaned)
                        # Duplicate tracking: if an anchor already exists, add numbered variants
                        if norm_clean in anchors:
                            # Count existing numbered variants to assign next index
                            existing_indices = [0]
                            for existing in list(anchors):
                                if existing == norm_clean:
                                    existing_indices.append(0)
                                elif re.match(rf"^{re.escape(norm_clean)}-(\d+)$", existing):
                                    try:
                                        existing_indices.append(int(existing.rsplit("-", 1)[1]))
                                    except Exception:
                                        pass
                            next_idx = max(existing_indices) + 1
                            numbered = f"{norm_clean}-{next_idx}"
                            anchors.add(numbered)
                        anchors.add(norm_clean)
                    # Also add normalized form of raw anchors (in case users link using normalized visible text form)
                    for a in HTML_ANCHOR_RE.finditer(heading_text):
                        raw = a.group(1).strip()
                        if raw:
                            anchors.add(github_anchor(raw))
    except Exception:
        pass
    ANCHOR_CACHE[path] = anchors
    return anchors


def is_http_url(url: str) -> bool:
    return url.startswith("http://") or url.startswith("https://")


def find_markdown_files(root: str) -> List[str]:
    files: List[str] = []
    for dirpath, _, filenames in os.walk(root):
        for fn in filenames:
            if fn.lower().endswith(".md"):
                files.append(os.path.join(dirpath, fn))
    return files


def parse_links(file_path: str) -> List[Tuple[int, str, str]]:
    links: List[Tuple[int, str, str]] = []
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            in_fence = False
            in_blockquote = False
            fence_delim = None  # track ``` or ~~~
            for idx, raw_line in enumerate(f, start=1):
                line = raw_line.rstrip("\n")
                # Detect start/end of fenced code blocks. Accept ``` or ~~~ with optional language.
                fence_match = re.match(r"^(?P<delim>`{3,}|~{3,})(.*)$", line)
                if fence_match:
                    full = fence_match.group("delim")
                    # Toggle if same delimiter starts/ends
                    if not in_fence:
                        in_fence = True
                        fence_delim = full
                        continue
                    else:
                        # Only close if same delimiter length & char
                        if fence_delim == full:
                            in_fence = False
                            fence_delim = None
                            continue
                if in_fence:
                    continue  # skip link detection inside code fences
                # Blockquote handling: if line starts with '>' treat entire following wrapped paragraph as quoted until blank line
                if re.match(r"^\s*>", line):
                    in_blockquote = True
                    continue
                if in_blockquote:
                    if line.strip() == "":
                        in_blockquote = False
                    else:
                        continue
                for m in LINK_RE.finditer(line):
                    text, target = m.group(1), m.group(2).strip()
                    links.append((idx, text, target))
    except Exception:
        pass
    return links


def validate_link(current_file: str, line: int, text: str, target: str) -> Optional[LinkIssue]:
    # Remove surrounding <> used sometimes in markdown
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]

    # Ignore empty link
    if target == "":
        return LinkIssue(current_file, line, text, target, "empty link target")

    # Fragment-only (#anchor)
    if target.startswith("#"):
        anchors = collect_headings(current_file)
        raw_anchor = target[1:]
        # Normalize link anchor the same way headings are normalized
        norm_anchor = github_anchor(raw_anchor)
        if norm_anchor not in anchors:
            # Fuzzy variants: attempt to tolerate missing or extra hyphens inside multi-token anchors.
            # Strategy:
            # 1. If anchor has hyphens, try removing each hyphen individually (concatenation forms).
            # 2. Try removing all hyphens (fully concatenated form).
            # 3. If anchor has N hyphens, also try forms with one extra hyphen inserted between adjacent alphanumerics
            #    (covers classic-plancache -> classic-plan-cache).
            # 4. If anchor has no hyphens, attempt inserting a hyphen at every internal boundary between alphanumerics.
            fuzzy_match = False
            variant_candidates: set[str] = set()
            a = norm_anchor
            # (1) remove each hyphen separately
            if "-" in a:
                for i, ch in enumerate(a):
                    if ch == "-":
                        variant_candidates.add(a[:i] + a[i + 1 :])
                # (2) remove all hyphens
                variant_candidates.add(a.replace("-", ""))
                # (3) insert extra hyphen between alphanumerics where not already hyphen
                for i in range(1, len(a)):
                    if a[i] != "-" and a[i - 1] != "-":
                        if a[i - 1].isalnum() and a[i].isalnum():
                            variant_candidates.add(a[:i] + "-" + a[i:])
            else:
                # (4) insert hyphen at every internal boundary
                for i in range(1, len(a)):
                    if a[i - 1].isalnum() and a[i].isalnum():
                        variant_candidates.add(a[:i] + "-" + a[i:])
            # Limit explosion: cap at 50 candidates
            if len(variant_candidates) > 50:
                variant_candidates = set(list(variant_candidates)[:50])
            for cand in variant_candidates:
                if cand in anchors:
                    fuzzy_match = True
                    break
            if fuzzy_match:
                return None  # Suppress issue since a fuzzy variant matches
            return LinkIssue(current_file, line, text, target, "anchor not found in this file")
        return None

    # Split fragment if present
    file_part, frag_part = target.split("#", 1) if "#" in target else (target, None)

    if is_http_url(file_part):
        # Allow detection of malformed scheme 'hhttps://' but otherwise skip external validation
        if file_part.startswith("hhttps://"):
            return LinkIssue(
                current_file, line, text, target, "malformed scheme (did you mean https:// ?)"
            )
        # Enforce pinned GitHub refs for mongodb/mongo and 10gen/mongo repositories.
        gh_match = re.match(
            r"^https://github.com/(mongodb|10gen)/mongo/(blob|tree)/([^/]+)/([^#]+)(?:#.*)?$",
            target,
        )
        if gh_match:
            owner, kind, ref, path_rest = gh_match.groups()
            if ref == "master":
                return LinkIssue(
                    current_file,
                    line,
                    text,
                    target,
                    "unpinned GitHub master reference; use tag/commit or relative path",
                )
            return None  # Non-master GitHub link accepted
        return None

    # Remove query params if any
    if "?" in file_part:
        parsed = urllib.parse.urlparse(file_part)
        file_part = parsed.path

    # Normalize relative path. If path starts with '/' treat as repo-root relative.
    repo_root = REPO_ROOT  # resolved once; works under Bazel runfiles
    if file_part.startswith("/"):
        resolved_path = os.path.normpath(os.path.join(repo_root, file_part.lstrip("/")))
    else:
        current_dir = os.path.dirname(current_file)
        resolved_path = os.path.normpath(os.path.join(current_dir, file_part))

    if not os.path.exists(resolved_path):
        return LinkIssue(current_file, line, text, target, f"file does not exist: {resolved_path}")

    if frag_part:
        # If target file is NOT markdown and fragment matches a GitHub line anchor (#Lnn or #Lnn-Lmm), accept.
        if not resolved_path.lower().endswith(".md") and re.match(r"^L\d+(-L\d+)?$", frag_part):
            return None
        anchors = (
            collect_headings(resolved_path) if resolved_path.lower().endswith(".md") else set()
        )
        if resolved_path.lower().endswith(".md"):
            norm_frag = github_anchor(frag_part)
            if norm_frag not in anchors:
                return LinkIssue(
                    current_file,
                    line,
                    text,
                    target,
                    f'anchor "{frag_part}" not found in target file',
                )
        else:
            # Non-markdown + non line-fragment: cannot validate anchor, assume ok.
            return None

    return None


def lint_files(files: Iterable[str], workers: int) -> List[LinkIssue]:
    issues: List[LinkIssue] = []

    def process(file_path: str) -> List[LinkIssue]:
        file_issues: List[LinkIssue] = []
        links = parse_links(file_path)
        for line, text, target in links:
            issue = validate_link(file_path, line, text, target)
            if issue:
                file_issues.append(issue)
        return file_issues

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as exe:
        futures = {exe.submit(process, f): f for f in files}
        for fut in concurrent.futures.as_completed(futures):
            for iss in fut.result():
                issues.append(iss)
    return issues


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Markdown link linter for src/mongo markdown files.")
    ap.add_argument("--root", default="src/mongo", help="Root directory to scan")
    ap.add_argument(
        "--workers",
        type=int,
        default=min(32, (os.cpu_count() or 4)),
        help="Parallel worker threads",
    )
    ap.add_argument("--json", action="store_true", help="Output machine-readable JSON")
    ap.add_argument("--verbose", action="store_true", help="Verbose output")
    ap.add_argument(
        "--auto-fix",
        action="store_true",
        help="Attempt automatic fixes for simple broken links (renames)",
    )
    ap.add_argument(
        "--rename-map",
        action="append",
        metavar="OLD=NEW",
        help="Directory/file rename mapping, e.g. catalog=local_catalog (can be repeated)",
    )
    ap.add_argument(
        "--search-moved",
        action="store_true",
        help="Search for missing file basenames under root and rewrite link if unique match found",
    )
    args = ap.parse_args(argv)

    root = args.root
    if not os.path.isdir(root):
        # Try resolving relative to detected repo root
        candidate = os.path.join(REPO_ROOT, root.lstrip("/"))
        if os.path.isdir(candidate):
            root = candidate
        else:
            print(
                f"Error: root directory {root} not found (repo root: {REPO_ROOT})", file=sys.stderr
            )
            return 1

    files = find_markdown_files(root)
    if args.verbose:
        print(f"Scanning {len(files)} markdown files under {root} ...")

    issues = lint_files(files, args.workers)

    # Moved-file search index (basename -> list of full paths). We walk the entire
    # root tree to include non-markdown sources (e.g., .h/.cpp) since links may point to headers.
    # Auto-enabled when --auto-fix is used; can also be explicitly enabled with --search-moved.
    moved_index: dict[str, list[str]] = {}
    if args.auto_fix or args.search_moved:
        for dirpath, dirnames, filenames in os.walk(root):
            # Avoid descending into very large generated output dirs if present.
            # (Heuristic: skip bazel-* dirs under root scan to reduce noise.)
            dirnames[:] = [d for d in dirnames if not d.startswith("bazel-")]
            for fn in filenames:
                if fn.startswith("."):
                    continue
                full = os.path.join(dirpath, fn)
                moved_index.setdefault(fn, []).append(full)
        if args.verbose:
            total_paths = sum(len(v) for v in moved_index.values())
            print(
                f"Built moved-file index: {len(moved_index)} unique basenames mapped to {total_paths} file(s)"
            )

    # Auto-fix pass (only for missing file issues with rename hints)
    if args.auto_fix and issues:
        rename_pairs = {}
        for pair in args.rename_map or []:
            if "=" in pair:
                old, new = pair.split("=", 1)
                rename_pairs[old.strip()] = new.strip()

        if rename_pairs and args.verbose:
            print(f"Auto-fix: applying rename map {rename_pairs}")

        fix_count = 0
        # Group issues by file for editing
        issues_by_file: dict[str, List[LinkIssue]] = {}
        for iss in issues:
            issues_by_file.setdefault(iss.file, []).append(iss)

        # Precompute anchor -> candidate files map to help relocation of anchors.
        anchor_index: dict[str, list[str]] = {}

        def index_file_anchors(path: str):
            if not path.lower().endswith(".md"):
                return
            for a in collect_headings(path):
                anchor_index.setdefault(a, []).append(path)

        # Index only when we encounter first anchor issue to keep performance reasonable.
        anchor_index_built = False

        for md_file, file_issues in issues_by_file.items():
            # Only attempt if file exists and we have rename hints
            if not os.path.isfile(md_file):
                continue
            try:
                # Use a distinct variable name (fh) for the file handle to avoid
                # shadowing earlier loop variables (e.g., 'f' used for file paths),
                # which was confusing the type checker.
                with open(md_file, "r", encoding="utf-8") as fh:
                    lines = fh.readlines()
            except Exception:
                continue

            # Deduplicate identical (message, target) to avoid repeated work (retain first occurrence)
            seen_sig = set()
            deduped: List[LinkIssue] = []
            for iss in file_issues:
                sig = (iss.message, iss.target)
                if sig in seen_sig:
                    continue
                seen_sig.add(sig)
                deduped.append(iss)

            modified = False
            for iss in deduped:
                # Always capture the current target early to avoid scope issues
                original_target = iss.target

                # 0. GitHub master link auto-fix: rewrite to repo-root relative path
                if "unpinned GitHub master reference" in iss.message:
                    m_gh = re.match(
                        r"^https://github.com/(mongodb|10gen)/mongo/(blob|tree)/master/([^#]+)(?:#(.*))?$",
                        original_target,
                    )
                    if m_gh:
                        path_part = m_gh.group(
                            3
                        )  # path inside repository (corrected: group 3, not 2)
                        frag_only = m_gh.group(4)  # fragment (corrected: group 4, not 3)
                        # GitHub URLs point to any repo path (src/, buildscripts/, jstests/, etc)
                        # All must become absolute repo-root refs like /buildscripts/... not buildscripts/...
                        new_target = "/" + path_part
                        if frag_only:
                            new_target += "#" + frag_only  # append single fragment only
                        for idx, line_text in enumerate(lines):
                            token = f"]({original_target})"
                            if token in line_text:
                                lines[idx] = line_text.replace(token, f"]({new_target})", 1)
                                modified = True
                                fix_count += 1
                                if args.verbose:
                                    print(
                                        f"Auto-fixed GitHub master link in {md_file}: {original_target} -> {new_target}"
                                    )
                                break

                # 1. Scheme / common typo fixes
                if "malformed scheme" in iss.message and original_target.startswith("hhttps://"):
                    fixed_target = original_target.replace("hhttps://", "https://", 1)
                    for idx, line_text in enumerate(lines):
                        if f"]({original_target})" in line_text:
                            lines[idx] = line_text.replace(
                                f"]({original_target})", f"]({fixed_target})", 1
                            )
                            modified = True
                            fix_count += 1
                            if args.verbose:
                                print(
                                    f"Auto-fixed malformed scheme in {md_file}: {original_target} -> {fixed_target}"
                                )
                            break

                # 2. Common directory typo fix (storgae -> storage)
                if "file does not exist:" in iss.message and "storgae" in original_target:
                    fixed_target = original_target.replace("storgae", "storage")
                    if fixed_target != original_target:
                        for idx, line_text in enumerate(lines):
                            if f"]({original_target})" in line_text:
                                lines[idx] = line_text.replace(
                                    f"]({original_target})", f"]({fixed_target})", 1
                                )
                                modified = True
                                fix_count += 1
                                if args.verbose:
                                    print(
                                        f"Auto-fixed path typo in {md_file}: {original_target} -> {fixed_target}"
                                    )
                                break

                # 3. Anchor relocation: only attempt if we can extract a plausible fragment token
                if ('anchor "' in iss.message and "not found in target file" in iss.message) or (
                    "anchor not found in this file" in iss.message
                ):
                    # Accept fragments comprised of word chars, dashes, underscores, and periods
                    m_anchor = re.search(r'anchor "([A-Za-z0-9_.:-]+)"', iss.message)
                    frag: Optional[str] = None
                    if m_anchor:
                        frag = m_anchor.group(1)
                    else:
                        # Fallback extraction ONLY from the original target if it starts with '#'
                        if original_target.startswith("#") and len(original_target) > 1:
                            frag = original_target[1:]
                        elif "#" in original_target:
                            frag = original_target.split("#", 1)[1]
                    # Guard against obviously wrong fragments like 'not' arising from message text
                    if frag and frag.lower() == "not":
                        frag = None
                    if frag:
                        norm_frag = github_anchor(frag)
                        if not anchor_index_built:
                            for fpath in files:
                                index_file_anchors(fpath)
                            anchor_index_built = True
                        candidates = anchor_index.get(norm_frag, [])
                        if not candidates and args.verbose:
                            print(
                                f'Verbose: no indexed candidates for anchor "{frag}" (normalized "{norm_frag}") referenced from {md_file}. Performing fallback scan...'
                            )
                            # Fallback: scan sibling and parent directories (one level up) for the anchor
                            search_dirs = {os.path.dirname(md_file)}
                            parent_dir = os.path.dirname(os.path.dirname(md_file))
                            if os.path.isdir(parent_dir):
                                search_dirs.add(parent_dir)
                            fallback_matches: list[str] = []
                            for d in list(search_dirs):
                                try:
                                    for fn in os.listdir(d):
                                        if fn.lower().endswith(".md"):
                                            candidate_path = os.path.join(d, fn)
                                            for a in collect_headings(candidate_path):
                                                if a == norm_frag:
                                                    fallback_matches.append(candidate_path)
                                                    break
                                except Exception:
                                    pass
                            if fallback_matches:
                                candidates = fallback_matches
                                if args.verbose:
                                    print(
                                        f'Verbose: fallback found {len(candidates)} candidate(s) for anchor "{frag}"'
                                    )
                        # Global one-time fallback: scan entire root if still no candidates
                        if not candidates:
                            # Perform a global scan only once per fragment per run (simple memo via anchor_index miss)
                            if args.verbose:
                                print(
                                    f'Verbose: performing global scan for anchor "{frag}" under root {root}'
                                )
                            try:
                                for gfile in files:
                                    if gfile.lower().endswith(".md"):
                                        if norm_frag in collect_headings(gfile):
                                            candidates.append(gfile)
                            except Exception:
                                pass
                            if candidates and args.verbose:
                                print(
                                    f'Verbose: global scan found {len(candidates)} candidate(s) for anchor "{frag}"'
                                )
                        if candidates:
                            chosen: Optional[str] = None
                            if len(candidates) == 1:
                                chosen = candidates[0]
                            else:
                                # Proximity heuristic: minimal directory distance (count of differing path segments)
                                base_dir = os.path.dirname(md_file)

                                def dir_distance(a: str, b: str) -> int:
                                    a_parts = os.path.abspath(a).split(os.sep)
                                    b_parts = os.path.abspath(b).split(os.sep)
                                    # Find common prefix length
                                    i = 0
                                    for x, y in zip(a_parts, b_parts):
                                        if x != y:
                                            break
                                        i += 1
                                    return (len(a_parts) - i) + (len(b_parts) - i)

                                # Rank by distance then by path length for stability
                                chosen = sorted(
                                    candidates, key=lambda p: (dir_distance(base_dir, p), len(p))
                                )[0]
                            if chosen:
                                rel_path = os.path.relpath(chosen, os.path.dirname(md_file))
                                new_target = f"{rel_path}#{frag}"
                                search_token = f"]({original_target})"
                                replaced_any = False
                                for idx, line_text in enumerate(lines):
                                    if search_token in line_text:
                                        # Replace only the first occurrence per line to avoid accidental nested replacements, but scan all lines.
                                        lines[idx] = line_text.replace(
                                            search_token, f"]({new_target})", 1
                                        )
                                        modified = True
                                        replaced_any = True
                                        fix_count += 1
                                if replaced_any and args.verbose:
                                    if len(candidates) > 1:
                                        print(
                                            f"Auto-relocated anchor (closest of {len(candidates)}) in {md_file}: {original_target} -> {new_target}"
                                        )
                                    else:
                                        print(
                                            f"Auto-relocated anchor in {md_file}: {original_target} -> {new_target}"
                                        )

                # 4. Path segment rename fixes (directory renames) independent of anchor relocation
                if rename_pairs and "file does not exist:" in iss.message:
                    path_part = original_target.split("#", 1)[0]
                    new_path_part = path_part
                    for old, new in rename_pairs.items():
                        pattern = re.compile(rf"(?:^|/)({re.escape(old)})(?=/|$)")
                        new_path_part = pattern.sub(
                            lambda m: m.group(0).replace(old, new), new_path_part
                        )
                    if new_path_part != path_part:
                        new_target = new_path_part + (
                            ""
                            if "#" not in original_target
                            else "#" + original_target.split("#", 1)[1]
                        )
                        for idx, line_text in enumerate(lines):
                            if f"]({original_target})" in line_text:
                                lines[idx] = line_text.replace(
                                    f"]({original_target})", f"]({new_target})", 1
                                )
                                modified = True
                                fix_count += 1
                                if args.verbose:
                                    print(
                                        f"Auto-fixed link in {md_file}: {original_target} -> {new_target}"
                                    )
                                break
                # 5. Moved file basename search (auto-enabled with --auto-fix)
                if "file does not exist:" in iss.message and "#" not in original_target:
                    # Extract the basename of the missing file
                    missing_base = os.path.basename(original_target)
                    # Skip obviously non-file references (contain spaces or wildcard characters)
                    # Allow basename-only references (original_target may equal missing_base)
                    if missing_base and " " not in missing_base:
                        candidates = moved_index.get(missing_base, [])
                        # If no candidates under the provided root, attempt a one-time scan of the
                        # full repo root (this can be expensive, so only do it when we miss locally).
                        if not candidates:
                            global_hits: list[str] = []
                            for dirpath, dirnames, filenames in os.walk(REPO_ROOT):
                                # Skip bazel output directories to reduce noise.
                                dirnames[:] = [d for d in dirnames if not d.startswith("bazel-")]
                                if missing_base in filenames:
                                    global_hits.append(os.path.join(dirpath, missing_base))
                                # Fast exit if >1 found (ambiguity)
                                if len(global_hits) > 1:
                                    break
                            if len(global_hits) == 1:
                                candidates = global_hits
                            if args.verbose:
                                if not global_hits:
                                    print(
                                        f"Verbose: moved-file search found no global candidates for {missing_base} (original target {original_target})"
                                    )
                                elif len(global_hits) > 1:
                                    print(
                                        f"Verbose: moved-file search ambiguous ({len(global_hits)} matches) for {missing_base}; skipping auto-fix"
                                    )
                                else:
                                    print(
                                        f"Verbose: moved-file global search matched unique file {global_hits[0]} for {missing_base}"
                                    )
                        if len(candidates) == 1:
                            target_file_candidate = candidates[0]
                            rel_path = os.path.relpath(
                                target_file_candidate, os.path.dirname(md_file)
                            )
                            new_target = rel_path
                            for idx, line_text in enumerate(lines):
                                token = f"]({original_target})"
                                if token in line_text:
                                    lines[idx] = line_text.replace(token, f"]({new_target})", 1)
                                    modified = True
                                    fix_count += 1
                                    if args.verbose:
                                        print(
                                            f"Auto-fixed moved file in {md_file}: {original_target} -> {new_target}"
                                        )
                                    break
            if modified:
                try:
                    with open(md_file, "w", encoding="utf-8") as fh:
                        fh.writelines(lines)
                except Exception:
                    print(f"Warning: failed to write fixes to {md_file}", file=sys.stderr)

        if args.verbose:
            print(f"Auto-fix completed: {fix_count} link(s) updated")
        # Re-run lint to update issues list after fixes
        if fix_count:
            ANCHOR_CACHE.clear()
            issues = lint_files(files, args.workers)

    if args.json:
        print(json.dumps([i.to_dict() for i in issues], indent=2))
    else:
        for issue in issues:
            print(f"{issue.file}:{issue.line}: {issue.message} [{issue.target}]")

    if issues:
        print(f"Found {len(issues)} markdown link issue(s).", file=sys.stderr)
        return 2
    else:
        if args.verbose:
            print("All links OK.")
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
