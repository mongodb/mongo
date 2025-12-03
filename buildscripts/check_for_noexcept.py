#!/usr/bin/env python3
"""
Check for changes in noexcept usage between the given revisions and output a file that
can be used by evergreen to post a github commit check annotating a PR with the results.

The algorithm here handles a few cases: (1) straightforward cases where you're "just"
adding or removing noexcept from an existing function; (2) some cases where you're adding
a new noexcept function or functions; (3) cases where you're adding a non-noexcept move
constructor or assignment operation. Case (1) works like this: if we have a diff block
(that is, a block of addition or deletion) that's only mentions noexcept on only one side,
those mentions are not detected as move operations, and the only diff between the two
sides is related to the noexcept(s), count it. Case (2) works like this: if noexcept is
mentioned more times on the b-side of the diff than the a-side (excluding move
operations), assume new noexcept code has been added, and count it. Case (3) simply
detects added code that looks like a move operation and alerts if it isn't noexcept.
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
ANNOTATION_TITLE_TEXT_NOEXCEPT_CHANGE = TITLE_TEXT
ANNOTATION_MESSAGE_TEXT_NOEXCEPT_CHANGE = (
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

ANNOTATION_TITLE_TEXT_NON_NOEXCEPT_MOVE = "Non-noexcept move operation added"
ANNOTATION_MESSAGE_TEXT_NON_NOEXCEPT_MOVE = (
    "A non-noexcept move operation has been added in this change.\n"
    "Move operations should generally be marked noexcept to\n"
    "avoid unnecessary copies during standard library operations.\n"
    "If the move operation truly can throw and is intentionally not\n"
    "noexcept, please disregard this message. "
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


def get_matching_blocks(lhs: str, rhs: str) -> list[tuple[int, int, int, int]]:
    """
    Find the (line-level) matching blocks between the two given strings, representing the left-hand
    side (lhs) and right-hand side (rhs) of a file with a diff. That is, they represent the file's
    contents before (lhs) and after (rhs) the change. Return value is a tuple of (lhs_start,
    lhs_end, rhs_start, rhs_end) for each matching block. Unlike difflib's
    SequenceMatcher.get_matching_blocks, we do not return the dummy size-0 block at the end.
    """
    lhs_lines = lhs.splitlines()
    rhs_lines = rhs.splitlines()
    matcher = difflib.SequenceMatcher(None, lhs_lines, rhs_lines, autojunk=False)
    blocks = matcher.get_matching_blocks()
    line_diffs = []
    for block in blocks:
        if block.size == 0:
            # The last block is a dummy block a and b set to the sequence lengths and size 0.
            # We don't return such a block since we don't need it.
            break
        lhs_start = block.a
        rhs_start = block.b
        line_diffs.append((lhs_start, lhs_start + block.size, rhs_start, rhs_start + block.size))
    return line_diffs


def get_nonmatching_blocks(lhs: str, rhs: str) -> list[tuple[int, int, int, int]]:
    matches = get_matching_blocks(lhs, rhs)
    lhs_index = 0
    rhs_index = 0
    blocks = []
    lhs_line_count = len(lhs.splitlines())
    rhs_line_count = len(rhs.splitlines())
    for lhs_start, lhs_end, rhs_start, rhs_end in matches:
        if lhs_index < lhs_start or rhs_index < rhs_start:
            blocks.append((lhs_index, lhs_start, rhs_index, rhs_start))
        lhs_index = lhs_end
        rhs_index = rhs_end
    if lhs_index < lhs_line_count or rhs_index < rhs_line_count:
        blocks.append((lhs_index, lhs_line_count, rhs_index, rhs_line_count))
    return blocks


def get_line_for_char(text: str, i: int) -> int:
    # The first line (before any newlines) is line 1 rather than line 0.
    return text[:i].count("\n") + 1


def strip_noexcept(s: str) -> str:
    return re.sub(r"\snoexcept(\s*\(.*\))?(?=\s|;|{|$)", "", s)


class SnippetKind(Enum):
    # Noexcept is the most general kind of snippet, it simply refers to an instance of noexcept
    # that wasn't classified as more specific type of snippet.
    Noexcept = auto()
    # A NoexceptAddition is a snippet representing a definite addition of noexcept that wasn't
    # classified as a move.
    NoexceptAddition = auto()
    # A NoexceptRemoval is a snippet representing a definite removal of noexcept that wasn't
    # classified as a move.
    NoexceptRemoval = auto()
    # A NoexceptMove is a move operation that is marked noexcept.
    NoexceptMove = auto()
    # A NonNoexceptMove is a move operation that is not marked noexcept.
    NonNoexceptMove = auto()


@dataclass
class UnboundSnippet:
    kind: SnippetKind
    start: int
    end: int
    identifier: str | None

    def is_move(self) -> bool:
        return self.kind in (SnippetKind.NoexceptMove, SnippetKind.NonNoexceptMove)

    # We take a separate alert line since snippets on the LHS that are alertable need to be attached
    # to a line on the RHS. If it's not provided, we just use the start line of the snippet,
    # under the assumption that it's from the RHS.
    def _bind_to_text(self, s: str, alert_line: int | None = None):
        start = get_line_for_char(s, self.start)
        end = get_line_for_char(s, self.end)
        if alert_line is None:
            alert_line = start
        return LineBoundSnippet(
            **asdict(self), line_start=start, line_end=end, alert_line=alert_line
        )

    def bind_to_rhs(self, s: str):
        return self._bind_to_text(s)

    def bind_to_lhs(self, s: str, alert_line: int):
        return self._bind_to_text(s, alert_line=alert_line)


@dataclass
class LineBoundSnippet(UnboundSnippet):
    line_start: int
    line_end: int
    alert_line: int

    def bind_to_file(self, file: str):
        return FileBoundSnippet(**asdict(self), file=file)


@dataclass
class FileBoundSnippet(LineBoundSnippet):
    file: str


class AlertKind(Enum):
    NonNoexceptMove = auto()
    Noexcept = auto()


@dataclass
class Alert:
    file: str
    line: int
    kind: AlertKind
    identifier: str | None

    def get_title(self) -> str:
        if self.kind == AlertKind.Noexcept:
            return ANNOTATION_TITLE_TEXT_NOEXCEPT_CHANGE
        else:
            assert self.kind == AlertKind.NonNoexceptMove
            return ANNOTATION_TITLE_TEXT_NON_NOEXCEPT_MOVE

    def get_message(self) -> str:
        if self.kind == AlertKind.Noexcept:
            return ANNOTATION_MESSAGE_TEXT_NOEXCEPT_CHANGE
        else:
            assert self.kind == AlertKind.NonNoexceptMove
            return ANNOTATION_MESSAGE_TEXT_NON_NOEXCEPT_MOVE

    def make_annotation(self) -> dict[str, Any]:
        return {
            "path": self.file,
            "annotation_level": "notice",
            "title": self.get_title(),
            "message": self.get_message(),
            "raw_details": "",
            "start_line": self.line,
            "end_line": self.line,
        }


def get_noexcepts(s: str) -> list[UnboundSnippet]:
    noexcept_patterns = [
        re.compile(r"\bnoexcept\b"),
    ]
    noexcepts = []
    for pattern in noexcept_patterns:
        for match in pattern.finditer(s):
            snippet = UnboundSnippet(
                start=match.start(),
                end=match.end(),
                identifier=None,
                kind=SnippetKind.Noexcept,
            )
            noexcepts.append(snippet)
    return noexcepts


def _get_all_move_operations(s: str) -> list[tuple[int, int, str]]:
    """
    Find all move constructors and assignment operators in the given string. Returns a list of
    (start_char, end_char, identifier) triples, where identifier is what we detected as the
    name of the move operation (e.g., TypeName::TypeName for a move constructor or
    TypeName::operator= for a move assignment).
    """
    move_constructor_patterns = [
        # TypeName(TypeName&&...) - TypeName can be anything capitalized and we don't check the two
        # instances of Typename are the same. We also don't check that TypeName is the name of the
        # type, so this could be a normal function with a bad name. We do try to check that there
        # are no additional parameters beyond the move parameter.
        re.compile(r"\b[A-Z]\w*\s*\(([A-Z]\w*)\s*&&[^,)]*\)", re.MULTILINE),
    ]
    move_assignment_patterns = [
        # operator=(Typename&&...) - TypeName can be anything capitalized and we do try to check
        # that there are no additional parameters beyond the move parameter.
        re.compile(r"\boperator=\s*\(([A-Z]\w*)\s*&&[^,)]*\)", re.MULTILINE),
    ]

    move_operations = []

    for pattern in move_constructor_patterns:
        for match in pattern.finditer(s):
            move_operations.append(
                (match.start(), match.end(), match.group(1) + "::" + match.group(1))
            )

    for pattern in move_assignment_patterns:
        for match in pattern.finditer(s):
            move_operations.append((match.start(), match.end(), match.group(1) + "::operator="))

    return move_operations


def get_move_operations(s: str) -> list[UnboundSnippet]:
    """
    Find and return all move operations (constructors or assignment operators) in the given string.
    """
    matches = _get_all_move_operations(s)
    snippets = []
    for start, end, identifier in matches:
        # end is the closing bracket of the move operation's parameter list.
        # We look for the noexcept token between there and the next ;, {, or end of string,
        # and we'll also extend the snippet to that point if noexcept is found.
        move_op_end = min([s.find(c, end) if c in s else len(s) for c in (";", "{")])
        trailing_text = s[end:move_op_end]
        is_noexcept = "noexcept" in trailing_text
        snippet_kind = SnippetKind.NoexceptMove if is_noexcept else SnippetKind.NonNoexceptMove
        snippet_end = move_op_end if is_noexcept else end
        snippet = UnboundSnippet(
            start=start,
            end=snippet_end,
            identifier=identifier,
            kind=snippet_kind,
        )
        snippets.append(snippet)

    return snippets


def filter_snippets(
    snippets: list[LineBoundSnippet], start: int, end: int
) -> list[LineBoundSnippet]:
    def is_in_range(snippet: LineBoundSnippet, start: int, end: int) -> bool:
        return start <= snippet.line_start <= end or start <= snippet.line_end <= end

    return [s for s in snippets if is_in_range(s, start, end)]


def drop_covered_noexcept_snippets(snippets: list[LineBoundSnippet]) -> list[LineBoundSnippet]:
    """
    Remove any snippets of less-specific types (Noexcept, NoexceptAddition, NoexceptRemoval) that
    are fully covered by snippets of more-specific types (NoexceptMove).
    """

    noexcept_moves = [s for s in snippets if s.kind == SnippetKind.NoexceptMove]
    filtered = []
    for snippet in snippets:
        if snippet.is_move():
            filtered.append(snippet)
            continue
        is_covered = any(
            move_op.start <= snippet.start <= move_op.end for move_op in noexcept_moves
        )
        if not is_covered:
            filtered.append(snippet)
    return filtered


def analyze_text_diff(lhs: str, rhs: str) -> tuple[list[LineBoundSnippet], list[LineBoundSnippet]]:
    """
    Analyze the text diff between lhs and rhs, returning snippets of noexcept-relevant changes.
    Returns two lists: snippets from lhs and snippets from rhs.
    """

    def normalize(s: str) -> str:
        return re.sub(r"\s+", "", s)

    line_diff = get_nonmatching_blocks(lhs, rhs)

    # Get all snippets, considering the whole file. We'll filter down to those that overlap with
    # each diff block later. This ensures that when finding snippets, we have enough context.
    # For example, if there's code like:
    #    TypeName& operator=(TypeName&& other)
    #        noexcept {
    # and the diff only covers the "noexcept {" line, we still want to look at the line before
    # to realize it's a move assignment operator.
    all_pre_snippets = drop_covered_noexcept_snippets(get_noexcepts(lhs) + get_move_operations(lhs))
    all_post_snippets = drop_covered_noexcept_snippets(
        get_noexcepts(rhs) + get_move_operations(rhs)
    )

    pre, post = [], []
    for lstart, lend, rstart, rend in line_diff:
        # Ensure snippets on the LHS generate alerts on the corresponding RHS line, as alerts
        # can only appear on the RHS. Snippets already on the RHS just use their own line.
        pre_snippets = [s.bind_to_lhs(lhs, rstart) for s in all_pre_snippets]
        post_snippets = [s.bind_to_rhs(rhs) for s in all_post_snippets]

        # Filter snippets down to those that overlap with this diff block.
        pre_snippets = filter_snippets(pre_snippets, lstart, lend)
        post_snippets = filter_snippets(post_snippets, rstart, rend)

        rhs_text = " ".join(rhs.splitlines()[rstart:rend])
        lhs_text = " ".join(lhs.splitlines()[lstart:lend])

        # Special case: if there are snippets on only one side, they're all of type Noexcept,
        # and the only textual difference between the two sides is the presence or absence of
        # noexcept, we can promote those snippets to NoexceptAddition or NoexceptRemoval.
        # This is the case where we can definitively tell that noexcept was added to or removed
        # from existing code, and that code isn't a move operation (probably, up to the accuracy
        # of our move detection).
        if (
            len(pre_snippets) == 0
            and len(post_snippets) > 0
            and all(s.kind == SnippetKind.Noexcept for s in post_snippets)
            and normalize(strip_noexcept(rhs_text)) == normalize(lhs_text)
        ):
            for s in post_snippets:
                s.kind = SnippetKind.NoexceptAddition

        if (
            len(post_snippets) == 0
            and len(pre_snippets) > 0
            and all(s.kind == SnippetKind.Noexcept for s in pre_snippets)
            and normalize(strip_noexcept(lhs_text)) == normalize(rhs_text)
        ):
            for s in pre_snippets:
                s.kind = SnippetKind.NoexceptRemoval

        pre += pre_snippets
        post += post_snippets

    return pre, post


def is_allowed_path(path: str) -> bool:
    if not path.startswith("src/mongo"):
        return False
    if not (path.endswith(".h") or path.endswith(".cpp")):
        return False
    return True


def analyze_single_git_diff(
    diff: git.Diff,
) -> tuple[list[LineBoundSnippet], list[LineBoundSnippet]]:
    # Blobs can be None. For example, if the diff is a file deletion, the b_blob will be None.
    # In that case, we want the None to be treated as empty string for our text diff.
    def decode(blob: git.Blob | None) -> str:
        if not blob:
            return ""
        return blob.data_stream.read().decode("utf-8")

    if not is_allowed_path(diff.a_path):
        return [], []
    if not is_allowed_path(diff.b_path):
        return [], []

    return analyze_text_diff(decode(diff.a_blob), decode(diff.b_blob))


def analyze_git_diff(
    diffs: list[git.Diff],
) -> tuple[list[FileBoundSnippet], list[FileBoundSnippet]]:
    pre, post = [], []
    for diff in diffs:
        file_pre, file_post = analyze_single_git_diff(diff)
        pre += [change.bind_to_file(diff.a_path) for change in file_pre]
        post += [change.bind_to_file(diff.b_path) for change in file_post]

    return pre, post


def analyze_changes(pre: list[FileBoundSnippet], post: list[FileBoundSnippet]) -> list[Alert]:
    """
    Given pre- and post-change snippets, analyze the, and return any alerts that should be raised.
    """
    alerts = []

    # First, check for non-noexcept moves being added (including the case where we remove noexcept
    # from an existing noexcept move).
    non_noexcept_moves_post = [s for s in post if s.kind == SnippetKind.NonNoexceptMove]
    non_noexcept_move_identifiers_pre = {
        s.identifier for s in pre if s.kind == SnippetKind.NonNoexceptMove
    }
    for s in non_noexcept_moves_post:
        if s.identifier in non_noexcept_move_identifiers_pre:
            # This was already a non-noexcept move, it's not the PR's fault.
            LOGGER.info("Skipping alert for existing non-noexcept move", identifier=s.identifier)
            continue
        # This is a new non-noexcept move (or noexcept was removed from an existing move) - alert.
        LOGGER.info("Alerting for non-noexcept move", identifier=s.identifier)
        alerts.append(
            Alert(
                file=s.file,
                line=s.alert_line,
                kind=AlertKind.NonNoexceptMove,
                identifier=s.identifier,
            )
        )

    # Ignore any move operations, noexcept or not. We've already checked for the bad case related
    # to moves, so now we just need to look at other non-move noexcept changes.
    non_move_pre = [s for s in pre if not s.is_move()]
    non_move_post = [s for s in post if not s.is_move()]

    # Now we should be left with snippets for non-move noexcept changes only. First, check for the
    # case where we can definitively detect adding or removing noexcept from existing code.
    alerted_for_noexcept_change = False
    for s in non_move_pre + non_move_post:
        if s.kind in (SnippetKind.NoexceptAddition, SnippetKind.NoexceptRemoval):
            if alerted_for_noexcept_change:
                LOGGER.info(
                    "Not alerting for definitive noexcept change (already alerted once)",
                    file=s.file,
                    line=s.alert_line,
                )
            else:
                LOGGER.info(
                    "Alerting for defintive noexcept change", file=s.file, line=s.alert_line
                )
                alerts.append(
                    Alert(
                        file=s.file,
                        line=s.alert_line,
                        kind=AlertKind.Noexcept,
                        identifier=s.identifier,
                    )
                )
            alerted_for_noexcept_change = True

    # If the count increased, then we're adding new noexcept usage somewhere (either in new code or
    # in making old code noexcept). We may not have been able to detect these as definitive
    # additions above. This could happen if e.g., code is both moved to a new file and made
    # noexcept, or if new code is written using noexcept. If the count descreased, we might just be
    # deleting code that happens to contain noexcept, so we won't alert in that case.
    if not alerted_for_noexcept_change and len(non_move_post) > len(non_move_pre):
        if alerted_for_noexcept_change:
            LOGGER.info("Not alerting for new noexcept usage (already alerted once)")
        else:
            LOGGER.info("Alerting for net new noexcept usage")
            alerts.append(
                Alert(
                    file=non_move_post[0].file,
                    line=non_move_post[0].alert_line,
                    kind=AlertKind.Noexcept,
                    identifier=s.identifier,
                )
            )
        alerted_for_noexcept_change = True

    return alerts


def make_check_result(alerts: list[Alert]) -> dict[str, Any]:
    annotations = [alert.make_annotation() for alert in alerts]
    return {
        "title": TITLE_TEXT,
        "summary": SUMMARY_TEXT,
        "text": "",
        "annotations": annotations,
    }


def check_for_noexcept_changes(diffs: list[git.Diff]) -> dict[str, Any]:
    pre, post = analyze_git_diff(diffs)
    alerts = analyze_changes(pre, post)
    return make_check_result(alerts)


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

    with open(output_path, "w") as f:
        json.dump(check_result, f, indent=4)


if __name__ == "__main__":
    typer.run(main)
