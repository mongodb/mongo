#!/usr/bin/env python3
"""
Check for changes in noexcept usage between the given revisions and output a file that
can be used by evergreen to post a github commit check annotating a PR with the results.

The algorithm here basically handles two cases: (1) straightforward cases where
you're "just" adding or removing noexcept from an existing function; and (2)
some cases where you're adding a new noexcept function or functions. Case (1)
works like this: if we have a diff block (that is, a block of addition or
deletion) that's a single line and mentions noexcept on only one side, count
it. Case (2) works like this: if noexcept is mentioned on the b-side of the
diff and not the a-side, assume new noexcept code has been added, and count it.
"""

import difflib
import json
import logging
import re
import sys
from dataclasses import asdict, dataclass
from enum import Enum, auto
from typing import Annotated, Any, Optional

import git
import structlog
import typer
from structlog.stdlib import LoggerFactory

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.getLogger(__name__)


DOCUMENTATION_URL = (
    "https://github.com/10gen/mongo/blob/master/docs/exception_architecture.md#using-noexcept"
)

# These fields go into the checks tab, they do not appear inline in the "Files Changed" tab.
# This appears whether or not the check flags any noexcept changes.
TITLE_TEXT = "Changes in noexcept usage"
SUMMARY_TEXT = (
    "This check looks for changes in noexcept usage in order to link users to the "
    "guidelines for its usage [here](" + DOCUMENTATION_URL + ") "
    "when such changes are identified."
)

# These fields go in the annotations in the "Files Changed" tab. These fields are rendered
# verbatim (i.e., markdown doesn't work).
ANNOTATION_TITLE_TEXT = TITLE_TEXT
ANNOTATION_MESSAGE_TEXT = (
    "A change in noexcept usage has been detected.\n"
    "As noexcept is a frequently-misunderstood feature,\n"
    "please refer to the documentation on noexcept usage\n"
    "linked below to evaluate if this change is appropriate. "
    "\n\n"
    "This message will only be posted once per PR, even if\n"
    "there are multiple detected changes to noexcept usage. "
    "\n\n"
    "This may be a false positive, such as on move operations.\n"
    "If so, disregard this message. "
    "\n\n"
    "Documentation can be found here: " + DOCUMENTATION_URL
)


def configure_logging() -> None:
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=logging.INFO,
        stream=sys.stderr,
    )


def get_merge_base(repo: git.Repo, base, rev) -> git.DiffIndex:
    merge_bases = repo.merge_base(rev, base)
    if len(merge_bases) == 0:
        raise RuntimeError(f"No common merge base between {base} and {rev}")
    assert len(merge_bases) == 1
    return merge_bases[0]


def get_git_diff(repo: git.Repo, base: git.Commit, rev) -> git.DiffIndex:
    return base.diff(rev)


class ChangeKind(Enum):
    ADDITION = auto()
    REMOVAL = auto()


@dataclass
class UnboundNoexceptChange:
    kind: ChangeKind
    is_certain: bool
    index: int

    def bind_to_line(self, line: int):
        return LineBoundNoexceptChange(**asdict(self), line=line)


@dataclass
class LineBoundNoexceptChange(UnboundNoexceptChange):
    line: int

    def bind_to_file(self, file: str):
        return FileBoundNoexceptChange(**asdict(self), file=file)


@dataclass
class FileBoundNoexceptChange(LineBoundNoexceptChange):
    file: str

    def make_annotation(self) -> dict[str, Any]:
        return {
            "path": self.file,
            "annotation_level": "notice",
            "title": ANNOTATION_TITLE_TEXT,
            "message": ANNOTATION_MESSAGE_TEXT,
            "raw_details": "",
            "start_line": self.line,
            "end_line": self.line,
        }


def get_line_for_char(text: str, i: int) -> int:
    # The first line (before any newlines) is line 1 rather than line 0.
    return text[:i].count("\n") + 1


def analyze_uncertain_insdel(insdel: str, kind: ChangeKind) -> list[UnboundNoexceptChange]:
    return [
        UnboundNoexceptChange(kind=kind, is_certain=False, index=match.start())
        for match in re.finditer("noexcept", insdel)
    ]


def analyze_insdel(insdel: str, kind: ChangeKind) -> list[UnboundNoexceptChange]:
    # If the inserted or deleted string is a single line, we'll assume the presence of noexcept
    # indicates a true insertion or deletion of noexcept.
    if insdel.count("noexcept") == 1 and insdel.count("\n") == 0:
        return [UnboundNoexceptChange(kind=kind, is_certain=True, index=insdel.find("noexcept"))]
    return analyze_uncertain_insdel(insdel, kind)


def analyze_deletion(deleted: str) -> list[UnboundNoexceptChange]:
    return analyze_insdel(deleted, ChangeKind.REMOVAL)


def analyze_insertion(inserted: str) -> list[UnboundNoexceptChange]:
    return analyze_insdel(inserted, ChangeKind.ADDITION)


def analyze_edit(deleted: str, inserted: str) -> list[UnboundNoexceptChange]:
    if "noexcept" not in inserted:
        return analyze_deletion(deleted)
    if "noexcept" not in deleted:
        return analyze_insertion(inserted)

    # Both sides mention noexcept - we have far less certainty.
    deletions = analyze_uncertain_insdel(deleted, ChangeKind.REMOVAL)
    insertions = analyze_uncertain_insdel(inserted, ChangeKind.ADDITION)

    return insertions + deletions


# This is a wrapper around difflib.SeqeuenceMatcher's get_matching_blocks.
# SequenceMatcher synchronizes the sequences quite aggressively when diffing a character sequence,
# often inserting a very small match in the middle of what we'd like to consider a single edit.
# For example, considering the strings "int f() noexcept const" and "int f() const",
# it might align them like this:
# int f() noexcept const
# int f()     c     onst
# producing two insertions of "noex" and "ept", making it hard for us to analyze individual edits.
# This wrapper performs the match at the level of words and translates back to a character-based
# match sequence. The matching ignores whitespace, so matching regions may be of different sizes
# on each side and may indeed differ in whitespace.
def get_matching_blocks(lhs: str, rhs: str) -> list[tuple[int, int, int, int]]:
    def skip_whitespace(s: str, i: int) -> int:
        while i < len(s) and s[i].isspace():
            i += 1
        return i

    def skip_word(s: str, word: str, i: int) -> int:
        for char in word:
            assert i < len(s)
            assert s[i] == char
            i += 1
        return i

    def advance(s: str, word: str, i: int) -> int:
        i = skip_whitespace(s, i)
        i = skip_word(s, word, i)
        i = skip_whitespace(s, i)
        return i

    lhs_words = lhs.split()
    rhs_words = rhs.split()
    s = difflib.SequenceMatcher(None, lhs_words, rhs_words, autojunk=False)
    matching_blocks = s.get_matching_blocks()
    lhs_word_index = 0
    lhs_char_index = 0
    rhs_word_index = 0
    rhs_char_index = 0
    new_blocks = []
    for block in matching_blocks:
        if block.size == 0:
            # The last block is a dummy block a and b set to the sequence lengths and size 0.
            # We don't return such a block since we don't need it.
            break

        while lhs_word_index < block.a:
            lhs_char_index = advance(lhs, lhs_words[lhs_word_index], lhs_char_index)
            lhs_word_index += 1
        while rhs_word_index < block.b:
            rhs_char_index = advance(rhs, rhs_words[rhs_word_index], rhs_char_index)
            rhs_word_index += 1

        block_lhs_start = lhs_char_index
        while lhs_word_index < block.a + block.size:
            lhs_char_index = advance(lhs, lhs_words[lhs_word_index], lhs_char_index)
            lhs_word_index += 1
        block_rhs_start = rhs_char_index
        while rhs_word_index < block.b + block.size:
            rhs_char_index = advance(rhs, rhs_words[rhs_word_index], rhs_char_index)
            rhs_word_index += 1

        new_blocks.append((block_lhs_start, lhs_char_index, block_rhs_start, rhs_char_index))

    return new_blocks


def get_nonmatching_blocks(lhs: str, rhs: str) -> list[tuple[int, int, int, int]]:
    matches = get_matching_blocks(lhs, rhs)
    lhs_i = 0
    rhs_i = 0
    blocks = []
    for lhs_start, lhs_end, rhs_start, rhs_end in matches:
        if lhs_i < lhs_start or rhs_i < rhs_start:
            blocks.append((lhs_i, lhs_start, rhs_i, rhs_start))
        lhs_i = lhs_end
        rhs_i = rhs_end
    if lhs_i < len(lhs) or rhs_i < len(rhs):
        blocks.append((lhs_i, len(lhs), rhs_i, len(rhs)))
    return blocks


def analyze_text_diff(lhs: str, rhs: str) -> list[LineBoundNoexceptChange]:
    blocks = get_nonmatching_blocks(lhs, rhs)
    changes = []
    for lhs_start, lhs_end, rhs_start, rhs_end in blocks:
        deleted = lhs[lhs_start:lhs_end]
        inserted = rhs[rhs_start:rhs_end]
        block_changes = analyze_edit(deleted, inserted)

        # Line number always refers to the rhs of the diff (post-changes). Annotations can only
        # be put on that side.
        rhs_start_line = get_line_for_char(rhs, rhs_start)

        # Indices are relative to the start of inserted/deleted, convert them to be relative
        # to lhs/rhs.
        for change in block_changes:
            if change.kind == ChangeKind.ADDITION:
                # For additions, we bind to the line where noexcept appears.
                change.index += rhs_start
                changes += [change.bind_to_line(get_line_for_char(rhs, change.index))]
            elif change.kind == ChangeKind.REMOVAL:
                # For removals, we bind to the start of the diff block due to the constraint that
                # annotations go on the right-hand side.
                change.index += lhs_start
                changes += [change.bind_to_line(rhs_start_line)]
    return changes


def is_allowed_path(path: str) -> bool:
    if not path.startswith("src/mongo"):
        return False
    if not (path.endswith(".h") or path.endswith(".cpp")):
        return False
    return True


def analyze_single_git_diff(diff: git.Diff) -> list[LineBoundNoexceptChange]:
    # Blobs can be None. For example, if the diff is a file deletion, the b_blob will be None.
    # In that case, we want the None to be treated as empty string for our text diff.
    def decode(blob: git.Blob | None) -> str:
        if not blob:
            return ""
        return blob.data_stream.read().decode("utf-8")

    if not is_allowed_path(diff.a_path):
        return []
    if not is_allowed_path(diff.b_path):
        return []

    return analyze_text_diff(decode(diff.a_blob), decode(diff.b_blob))


def analyze_git_diff(diffs: list[git.Diff]) -> list[FileBoundNoexceptChange]:
    changes = []
    for diff in diffs:
        diff_changes = analyze_single_git_diff(diff)
        changes += [change.bind_to_file(diff.b_path) for change in diff_changes]

    return changes


def make_check_result(changes: list[FileBoundNoexceptChange]) -> dict[str, Any]:
    annotations = [change.make_annotation() for change in changes]
    return {
        "title": TITLE_TEXT,
        "summary": SUMMARY_TEXT,
        "text": "",
        "annotations": annotations,
    }


def check_for_noexcept_changes(diffs: list[git.Diff]) -> dict[str, Any]:
    changes = analyze_git_diff(diffs)

    # If we're certain that noexcept was added or removed in some places, make annotations based
    # on the changes we're sure of.
    certain_changes = [change for change in changes if change.is_certain]
    if len(certain_changes) > 0:
        return make_check_result(certain_changes)

    # We've detected changes involving noexcept, but we're not certain that they're meaningful.
    # For example, removing a noexcept function altogether would be picked up as a noexcept
    # deletion, but it's not one we want to report on.
    # Conservatively, we'll only report on uncertain noexcept changes if there are additions of
    # noexcept with _no_ removals. This is still not perfect - we might pick up comments, or
    # someone declaring a noexcept function that already has a definition elsewhere, for example.
    additions = [change for change in changes if change.kind == ChangeKind.ADDITION]
    removals = [change for change in changes if change.kind == ChangeKind.REMOVAL]
    if len(additions) > 0 and len(removals) == 0:
        return make_check_result(additions)

    # There are either no changes to noexcept or a mix of added and removed noexcepts. We'd need a
    # deeper analysis to figure out the mixed case.
    return make_check_result([])


def main(
    revision: Annotated[
        str, typer.Argument(help="Git revision to check for noexcept changes")
    ] = "HEAD",
    base: Annotated[
        Optional[str], typer.Option("--base", "-b", help="Git revision to compare against")
    ] = None,
    output_path: Annotated[
        str, typer.Option("--output", "-o", help="Path at which to output github annotations")
    ] = "../github_annotations.json",
    max_annotations: Annotated[
        int, typer.Option("--limit", help="Maximum number of annotations to output")
    ] = 1,
):
    """
    Check for changes in noexcept usage between the given revisions and output a file that
    can be used by evergreen to post a github commit check annotating a PR with the results.
    """
    configure_logging()

    repo = git.Repo(".")
    if base is None:
        base = repo.head.commit
    merge_base = get_merge_base(repo, base, revision)
    diffs = get_git_diff(repo, merge_base, revision)

    LOGGER.info(
        "Checking for noexcept changes",
        base=str(base),
        merge_base=str(merge_base),
        revision=revision,
    )

    check_result = check_for_noexcept_changes(diffs)

    # Cap annotations to the given limit - we only want to output one in the commit queue, but
    # for testing it's useful to see all of them.
    if "annotations" in check_result:
        annotations = check_result.get("annotations", [])
        if max_annotations > 0 and len(annotations) > max_annotations:
            check_result["annotations"] = annotations[:max_annotations]
    with open(output_path, "w") as f:
        json.dump(check_result, f, indent=4)


if __name__ == "__main__":
    typer.run(main)
