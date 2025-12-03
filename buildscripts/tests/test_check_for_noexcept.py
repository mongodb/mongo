import unittest

from buildscripts.check_for_noexcept import (
    AlertKind,
    FileBoundSnippet,
    SnippetKind,
    analyze_changes,
    analyze_text_diff,
    get_matching_blocks,
    get_move_operations,
    get_noexcepts,
    get_nonmatching_blocks,
    strip_noexcept,
)


class TestGetMatchingBlocks(unittest.TestCase):
    def _check_blocks_cover_string(self, blocks: list[tuple[int, int]], s: str):
        lines = s.splitlines()
        if len(blocks) == 0:
            self.assertEqual(len(lines), 0)
            return

        prev_end = 0
        for block_start, block_end in sorted(blocks):
            self.assertEqual(block_start, prev_end)
            prev_end = block_end
        self.assertEqual(prev_end, len(lines))

    def test_matches_and_nonmatches_are_accurate(self):
        cases = (
            ("abc\nbcd\ncde\ndef", "abc\nwxy\ncde\ndef"),
            ("abc\nbcd\ncde\ndef", "abc\ncde\ndef"),
            ("abc\nbcd\ncde\ndef", "abc\nbcd\ncde\ndef\nxyz"),
        )
        for lhs, rhs in cases:
            matching = get_matching_blocks(lhs, rhs)
            nonmatching = get_nonmatching_blocks(lhs, rhs)
            self.assertGreater(len(matching), 0)
            self.assertGreater(len(nonmatching), 0)

            lhs_lines, rhs_lines = lhs.splitlines(), rhs.splitlines()
            for lhs_start, lhs_end, rhs_start, rhs_end in matching:
                block_lhs = "\n".join(lhs_lines[lhs_start:lhs_end])
                block_rhs = "\n".join(rhs_lines[rhs_start:rhs_end])
                self.assertEqual(block_lhs, block_rhs)
            lhs_blocks = []
            rhs_blocks = []
            for lhs_start, lhs_end, rhs_start, rhs_end in matching + nonmatching:
                lhs_blocks.append((lhs_start, lhs_end))
                rhs_blocks.append((rhs_start, rhs_end))
            self._check_blocks_cover_string(rhs_blocks, rhs)
            self._check_blocks_cover_string(lhs_blocks, lhs)

    def test_empty_strings(self):
        self.assertEqual(get_matching_blocks("", "nonempty"), [])
        self.assertEqual(get_matching_blocks("nonempty", ""), [])
        self.assertEqual(get_matching_blocks("", ""), [])

        self.assertEqual(get_nonmatching_blocks("", "nonempty"), [(0, 0, 0, 1)])
        self.assertEqual(get_nonmatching_blocks("nonempty", ""), [(0, 1, 0, 0)])
        self.assertEqual(get_nonmatching_blocks("", ""), [])

    def test_no_match(self):
        self.assertEqual(get_matching_blocks("a b c d", "e f g h"), [])
        self.assertEqual(get_nonmatching_blocks("a b c d", "e f g h"), [(0, 1, 0, 1)])

    def test_equal_strings(self):
        s = "a b c d"
        self.assertEqual(get_matching_blocks(s, s), [(0, 1, 0, 1)])
        self.assertEqual(get_nonmatching_blocks(s, s), [])

    def test_basic_multiline_edit(self):
        lhs = "a\nb\nc\nd"
        rhs = "a\nw\nx\nd"
        self.assertEqual(get_matching_blocks(lhs, rhs), [(0, 1, 0, 1), (3, 4, 3, 4)])
        self.assertEqual(get_nonmatching_blocks(lhs, rhs), [(1, 3, 1, 3)])

    def test_basic_multiline_delete(self):
        lhs = "a\nb\nc\nd"
        rhs = "a\nd"
        self.assertEqual(get_matching_blocks(lhs, rhs), [(0, 1, 0, 1), (3, 4, 1, 2)])
        self.assertEqual(get_nonmatching_blocks(lhs, rhs), [(1, 3, 1, 1)])

    def test_basic_multiline_insert(self):
        lhs = "a\nd"
        rhs = "a\nb\nc\nd"
        self.assertEqual(get_matching_blocks(lhs, rhs), [(0, 1, 0, 1), (1, 2, 3, 4)])
        self.assertEqual(get_nonmatching_blocks(lhs, rhs), [(1, 1, 1, 3)])


# The basic cases below end with semicolons (generally). We can generate similar cases ending with {
# or just without the semicolon to cover those cases.
def _generate_additional_cases(cases: list[str]) -> tuple[str]:
    new_cases = list(cases)
    for case in cases:
        if case.endswith(";"):
            new_cases.append(case[:-1] + " {")
            new_cases.append(case[:-1])
    return tuple(set(new_cases))


# The hardcoded cases all contain noexcept. We can generate non-noexcept cases by removing any
# noexcept tokens.
def _generate_non_noexcept_cases(cases: list[str]) -> tuple[str]:
    new_cases = [strip_noexcept(case) for case in cases]
    return tuple(set(new_cases))


_BASIC_NOEXCEPT_MOVE_CONSTRUCTORS = _generate_additional_cases(
    [
        "TypeName(TypeName&& other) const noexcept;",
        "TypeName(TypeName&& other) const noexcept(std::is_nothrow_constructible_v<TypeName>);",
        "TypeName(TypeName&& other) noexcept;",
        "TypeName(TypeName&& other) noexcept const;",
        "TypeName(TypeName&& other) noexcept(std::is_nothrow_constructible_v<TypeName>) const;",
        "Typenoexcept(Typenoexcept&& other) const noexcept;",
        "Typenoexcept(Typenoexcept&& other) noexcept;",
        "Typenoexcept(Typenoexcept&& other) noexcept const;",
    ]
)


_BASIC_NOEXCEPT_MOVE_ASSIGNMENTS = _generate_additional_cases(
    [
        "TypeName& operator=(TypeName&&) noexcept;",
        "TypeName& operator=(TypeName&&) noexcept(std::is_nothrow_constructible_v<TypeName>);",
        "TypeName& operator=(TypeName&&) noexcept { return *this; }",
        "TypeName& operator=(TypeName&&) noexcept const;",
        "TypeName& operator=(TypeName&&) noexcept const { return *this; }",
        "TypeName& operator=(TypeName&&) noexcept(std::is_nothrow_constructible_v<TypeName>) const { return *this; }",
        "TypeName& operator=(TypeName&&) const noexcept;",
        "TypeName& operator=(TypeName&&) const noexcept { return *this; }",
        "Typenoexcept& operator=(Typenoexcept&&) noexcept { return *this; }",
        "Typenoexcept& operator=(Typenoexcept&&) noexcept(std::is_nothrow_constructible_v<Typenoexcept>) { return *this; }",
    ]
)


_BASIC_NON_NOEXCEPT_MOVE_CONSTRUCTORS = _generate_non_noexcept_cases(
    _BASIC_NOEXCEPT_MOVE_CONSTRUCTORS
)


_BASIC_NON_NOEXCEPT_MOVE_ASSIGNMENTS = _generate_non_noexcept_cases(
    _BASIC_NOEXCEPT_MOVE_ASSIGNMENTS
)


_BASIC_NOEXCEPT_MOVE_OPERATIONS = (
    _BASIC_NOEXCEPT_MOVE_CONSTRUCTORS + _BASIC_NOEXCEPT_MOVE_ASSIGNMENTS
)


_BASIC_NON_NOEXCEPT_MOVE_OPERATIONS = (
    _BASIC_NON_NOEXCEPT_MOVE_CONSTRUCTORS + _BASIC_NON_NOEXCEPT_MOVE_ASSIGNMENTS
)


# Tricky statements that look similar-ish to move operations but are not.
_BASIC_NOEXCEPT_NON_MOVE_OPERATIONS = _generate_additional_cases(
    [
        "functionName(TypeName&&) noexcept;",
        "functionName(TypeName&& other) noexcept;",
        "functionName(TypeName&&) const noexcept;",
        "functionName(TypeName&&, int) const noexcept;",
        "functionName(TypeName&& other, int otherParam) noexcept;",
        "functionName() noexcept;",
        "TypeName() noexcept;",
        "TypeName(const TypeName&) noexcept;",
        "TypeName(const TypeName&) noexcept(std::is_trivially_constructible_v<TypeName>);",
        "TypeName(TypeName) noexcept;",
        "functionName(TypeName val) noexcept;",
        "functionName(TypeName& val) noexcept;",
        "functionName(TypeName&& val) noexcept;",
        "auto lambda = [x = std::move(y)]() noexcept { return x; }",
        "auto lambda = [](TypeName&& x) noexcept { return x; }",
        "[](TypeName&& x) noexcept { return x; }",
        "([](TypeName&& x) noexcept { return x; })()",
        "TypeName(TypeName&&, int otherParam) noexcept;",
        "TypeName operator+(const TypeName& other) noexcept;",
        "TypeName operator+(const TypeName& other) const noexcept;",
        "TypeName& operator*() const noexcept;",
    ]
)


_BASIC_NON_NOEXCEPT_NON_MOVE_OPERATIONS = _generate_non_noexcept_cases(
    _BASIC_NOEXCEPT_NON_MOVE_OPERATIONS
)


def _noexcept_moves_only(snippets):
    return [s for s in snippets if s.kind == SnippetKind.NoexceptMove]


def _nonnoexcept_moves_only(snippets):
    return [s for s in snippets if s.kind == SnippetKind.NonNoexceptMove]


def _make_snippet(
    *,
    file: str,
    kind: SnippetKind,
    identifier: str | None = None,
    line: int | None = None,
    line_start: int | None = None,
    line_end: int | None = None,
    alert_line: int | None = None,
):
    if line is not None:
        line_start = line
        line_end = line
    else:
        line_start = line_start
        line_end = line_end

    if alert_line is None:
        alert_line = line_start

    return FileBoundSnippet(
        file=file,
        kind=kind,
        start=0,
        end=0,
        identifier=identifier,
        line_start=line_start,
        line_end=line_end,
        alert_line=alert_line,
    )


class TestCheckForNoexcept(unittest.TestCase):
    def test_strip_noexcept(self):
        no_noexcept_cases = _BASIC_NON_NOEXCEPT_MOVE_OPERATIONS
        for case in no_noexcept_cases:
            self.assertEqual(strip_noexcept(case), case)

        self.assertEqual(
            strip_noexcept(
                "int f(SomeType<float> x) noexcept(std::is_nothrow_constructible_v<SomeType<float>>);"
            ),
            "int f(SomeType<float> x);",
        )
        self.assertEqual(
            strip_noexcept("int f(SomeType<float> x) noexcept const;"),
            "int f(SomeType<float> x) const;",
        )
        self.assertEqual(
            strip_noexcept("int f(SomeType<float> x) noexcept;"),
            "int f(SomeType<float> x);",
        )

    def test_no_text(self):
        pre, post = analyze_text_diff("", "")
        self.assertEqual(pre, [])
        self.assertEqual(post, [])

    def test_simple_noexcept_additions(self):
        pre = "int f(SomeType<float> x) const {"
        posts = (
            "int f(SomeType<float> x) noexcept const {",
            "int f(SomeType<float> x) noexcept {",
            "int f(SomeType<float> x) noexcept(std::is_nothrow_constructible_v<SomeType<float>>) {",
            "int func(SomeType<float> x) noexcept const {",
            "int f(SomeType<double> x) noexcept const {",
            "int f(SomeType<float> y) noexcept const {",
            "int f(SomeOtherType<float> x) noexcept const {",
        )
        for post in posts:
            before, after = analyze_text_diff(pre, post)
            self.assertEqual(len(before), 0)
            self.assertEqual(len(after), 1)
            snippet = after[0]
            self.assertFalse(snippet.is_move())
            self.assertEqual(snippet.start, post.find("noexcept"))
            self.assertEqual(snippet.end, snippet.start + len("noexcept"))
            self.assertEqual(snippet.line_start, 1)
            self.assertEqual(snippet.line_end, 1)

    def test_simple_removals(self):
        pre = "int f(SomeType<float> x) noexcept const {"
        posts = (
            "int f(SomeType<float> x) const {",
            "int f(SomeType<float> x) {",
            "int func(SomeType<float> x) const {",
            "int f(SomeType<double> x) const {",
            "int f(SomeType<float> y) const {",
            "int f(SomeOtherType<float> x) const {",
        )
        for post in posts:
            before, after = analyze_text_diff(pre, post)
            self.assertEqual(len(before), 1)
            self.assertEqual(len(after), 0)
            snippet = before[0]
            self.assertFalse(snippet.is_move())
            self.assertEqual(snippet.start, pre.find("noexcept"))
            self.assertEqual(snippet.end, snippet.start + len("noexcept"))
            self.assertEqual(snippet.line_start, 1)
            self.assertEqual(snippet.line_end, 1)

    def test_definite_noexcept_addition_removal(self):
        with_noexcept = """
int f() {
    return x * y;
}
int g(int z) const noexcept(std::is_nothrow_constructible_v<T>) {
    return z + 1;
}
void q() noexcept {
    // do nothing
}
"""
        without_noexcept = """
int f() {
    return x * y;
}
int g(int z) const {
    return z + 1;
}
void q() {
    // do nothing
}
"""
        # Case where noexcept was definitively removed.
        before, after = analyze_text_diff(with_noexcept, without_noexcept)
        self.assertEqual(len(before), 2)
        self.assertEqual(len(after), 0)
        for snippet in before:
            self.assertEqual(snippet.kind, SnippetKind.NoexceptRemoval)

        # Case where noexcept was definitively added.
        before, after = analyze_text_diff(without_noexcept, with_noexcept)
        self.assertEqual(len(before), 0)
        self.assertEqual(len(after), 2)
        for snippet in before:
            self.assertEqual(snippet.kind, SnippetKind.NoexceptRemoval)

    def test_simple_unchanged_with_noexcept(self):
        pre = "int f(SomeType<float> x) noexcept {"
        post = "float f(SomeOtherType<int> y) noexcept {"
        before, after = analyze_text_diff(pre, post)
        self.assertEqual(len(before), 1)
        self.assertEqual(before[0].kind, SnippetKind.Noexcept)
        self.assertEqual(len(after), 1)
        self.assertEqual(after[0].kind, SnippetKind.Noexcept)

    def test_simple_unrelated_changes(self):
        pre = "int f(SomeType<float> x) noexcept const {"
        posts = (
            "int f(SomeType<float> x) noexcept {",
            "int func(SomeType<float> x) noexcept const {",
            "int f(SomeType<double> x) noexcept const {",
            "int f(SomeType<float> y) noexcept const {",
            "int f(SomeOtherType<float> x) noexcept const {",
        )
        for post in posts:
            before, after = analyze_text_diff(pre, post)
            self.assertEqual(len(before), 1)
            self.assertEqual(before[0].kind, SnippetKind.Noexcept)
            self.assertEqual(len(after), 1)
            self.assertEqual(after[0].kind, SnippetKind.Noexcept)

    def test_simple_noexcept_move_addition(self):
        pre = "\n\nTypeName(TypeName&& other);"
        post = "\n\nTypeName(TypeName&& other)\nnoexcept;"
        before, after = analyze_text_diff(pre, post)
        self.assertEqual(len(before), 1)
        snippet = before[0]
        self.assertEqual(snippet.kind, SnippetKind.NonNoexceptMove)
        self.assertEqual(snippet.start, 2)
        self.assertEqual(snippet.end, pre.find(")") + 1)
        self.assertEqual(snippet.line_start, 3)
        self.assertEqual(snippet.line_end, 3)

        self.assertEqual(len(after), 1)
        snippet = after[0]
        self.assertEqual(snippet.kind, SnippetKind.NoexceptMove)
        self.assertEqual(snippet.start, 2)
        self.assertEqual(snippet.end, len(post) - 1)
        self.assertEqual(snippet.line_start, 3)
        self.assertEqual(snippet.line_end, 4)

    def test_simple_noexcept_move_removal(self):
        pre = "\n\nTypeName(TypeName&& other) noexcept {\n /* code */ }"
        post = "\n\nTypeName(TypeName&& other) {\n /* code */ }"
        before, after = analyze_text_diff(pre, post)
        self.assertEqual(len(before), 1)
        snippet = before[0]
        self.assertEqual(snippet.kind, SnippetKind.NoexceptMove)
        self.assertEqual(snippet.start, 2)
        self.assertEqual(snippet.end, pre.find("{"))
        self.assertEqual(snippet.line_start, 3)
        self.assertEqual(snippet.line_end, 3)

        self.assertEqual(len(after), 1)
        snippet = after[0]
        self.assertEqual(snippet.kind, SnippetKind.NonNoexceptMove)
        self.assertEqual(snippet.start, 2)
        self.assertEqual(snippet.end, post.find(")") + 1)
        self.assertEqual(snippet.line_start, 3)
        self.assertEqual(snippet.line_end, 3)

    def test_get_move_operations_noexcept_independent(self):
        # Basic cases
        with_move_operation = _BASIC_NOEXCEPT_MOVE_OPERATIONS + _BASIC_NON_NOEXCEPT_MOVE_OPERATIONS
        without_move_operation = (
            _BASIC_NOEXCEPT_NON_MOVE_OPERATIONS + _BASIC_NON_NOEXCEPT_NON_MOVE_OPERATIONS
        )

        # Trickier cases
        with_move_operation += (
            """
            class TypeName {
                TypeName(TypeName&&) noexcept;
            };
            """,
            """
            TypeName& operator=(TypeName&& other) noexcept {
                auto lambda = [x = std::move(y)]() noexcept { return x; };",
                z = lambda();
                return *this;
            }
            """,
            "TypeName(TypeName&&) { /* noexcept */ }",
            "TypeName(TypeName&&); // noexcept",
            "Typenoexcept(Typenoexcept&& varnoexcept);",
            "TypeName(TypeName&&); TypeName() noexcept;",
            "Typenoexcept& operator=(Typenoexcept&&) { return *this; }",
            """
            TypeName& operator=(TypeName&& other) {
                auto lambda = [x = std::move(y)]() noexcept { return x; };",
                z = lambda();
                return *this;
            }
            """,
        )

        without_move_operation += (
            """
            class TypeName {
                TypeName(const TypeName&);
                TypeName(TypeName);
                functionName(TypeName&&);
            };
            """,
        )

        for snippet in with_move_operation:
            all_moves = get_move_operations(snippet)
            self.assertEqual(len(all_moves), 1, msg=f"Expected move operation: {snippet}")

        for snippet in without_move_operation:
            all_moves = get_move_operations(snippet)
            self.assertEqual(len(all_moves), 0, msg=f"Expected no move operation in {snippet}")

    def test_get_move_operations_noexcept_matters(self):
        # Basic cases
        with_noexcept = _BASIC_NOEXCEPT_MOVE_OPERATIONS
        without_noexcept = _BASIC_NON_NOEXCEPT_MOVE_OPERATIONS

        # Trickier cases
        with_noexcept += (
            "Typenoexcept& operator=(Typenoexcept&&) noexcept { return *this; }",
            """
            TypeName& operator=(TypeName&& other) noexcept {
                auto lambda = [x = std::move(y)]() noexcept { return x; };",
                z = lambda();
                return *this;
            }
            """,
        )

        without_noexcept += (
            "TypeName(TypeName&&) { /* noexcept */ }",
            "TypeName(TypeName&&); // noexcept",
            "Typenoexcept(Typenoexcept&& varnoexcept);",
            "TypeName(TypeName&&); TypeName() noexcept;",
            "Typenoexcept& operator=(Typenoexcept&&) { return *this; }",
            """
            TypeName& operator=(TypeName&& other) {
                auto lambda = [x = std::move(y)]() noexcept { return x; };",
                z = lambda();
                return *this;
            }
            """,
        )

        for snippet in with_noexcept:
            all_moves = get_move_operations(snippet)
            noexcept = _noexcept_moves_only(all_moves)
            non_noexcept = _nonnoexcept_moves_only(all_moves)
            self.assertEqual(len(noexcept), 1, msg=f"Expected noexcept move operation: {snippet}")
            self.assertEqual(
                len(non_noexcept), 0, msg=f"Expected no non-noexcept move operation: {snippet}"
            )

        for snippet in without_noexcept:
            all_moves = get_move_operations(snippet)
            noexcept = _noexcept_moves_only(all_moves)
            non_noexcept = _nonnoexcept_moves_only(all_moves)
            self.assertEqual(
                len(noexcept), 0, msg=f"Expected no noexcept move operation: {snippet}"
            )
            self.assertEqual(
                len(non_noexcept), 1, msg=f"Expected non-noexcept move operation: {snippet}"
            )

    def test_get_move_operations_negative_cases(self):
        negative_cases = (
            _BASIC_NOEXCEPT_NON_MOVE_OPERATIONS + _BASIC_NON_NOEXCEPT_NON_MOVE_OPERATIONS
        )

        for snippet in negative_cases:
            all_moves = get_move_operations(snippet)
            self.assertEqual(len(all_moves), 0, msg=f"Expected no move operation: {snippet}")

    def test_get_noexcepts(self):
        with_noexcept = _BASIC_NOEXCEPT_MOVE_OPERATIONS + _BASIC_NOEXCEPT_NON_MOVE_OPERATIONS
        without_noexcept = (
            _BASIC_NON_NOEXCEPT_MOVE_OPERATIONS + _BASIC_NON_NOEXCEPT_NON_MOVE_OPERATIONS
        )

        for snippet in with_noexcept:
            self.assertGreater(
                len(get_noexcepts(snippet)), 0, msg=f"Expected noexcept in snippet: {snippet}"
            )

        for snippet in without_noexcept:
            self.assertEqual(
                len(get_noexcepts(snippet)), 0, msg=f"Expected no noexcept in snippet: {snippet}"
            )

    def test_move_identifiers(self):
        templates = (
            "{t}({t}&&){c}{n};",
            "{t}({t}&& {v}){c}{n};",
            "operator=({t}&&){c}{n};",
            "operator=({t}&& {v}){c}{n};",
            "{t}& operator=({t}&&){c}{n};",
            "{t}& operator=({t}&& {v}){c}{n};",
            "{t}({t}&&)\n{c}\n{n};",
            "operator=({t}&&)\n{c}{n};",
        )

        for template in templates:
            for const in (" const", ""):
                for noexcept in (" noexcept", "noexcept(condition)", ""):
                    for varname in ("other", "varnoexcept", ""):
                        for typename in ("TypeName", "Typenoexcept"):
                            case = template.format(t=typename, v=varname, c=const, n=noexcept)
                            all_moves = get_move_operations(case)
                            self.assertEqual(
                                len(all_moves), 1, msg=f"Expected move operation: {case}"
                            )
                            move = all_moves[0]
                            if "operator=" in template:
                                identifier = typename + "::operator="
                            else:
                                identifier = typename + "::" + typename
                            self.assertEqual(move.identifier, identifier)

    def test_analyze_changes_simple_addition_removal(self):
        # No alert on noexcept deletion.
        snippets = [
            _make_snippet(file="file1.cpp", kind=SnippetKind.Noexcept, line_start=10, line_end=10),
        ]
        alerts = analyze_changes(snippets, [])
        self.assertEqual(len(alerts), 0)

        # Alert on noexcept addition.
        alerts = analyze_changes([], snippets)
        self.assertEqual(len(alerts), 1)
        alert = alerts[0]
        self.assertEqual(alert.file, "file1.cpp")
        self.assertEqual(alert.kind, AlertKind.Noexcept)
        self.assertEqual(alert.line, 10)

    def test_analyze_changes_non_noexcept_move(self):
        other_snippets = [
            _make_snippet(
                file="file1.cpp", kind=SnippetKind.NoexceptMove, identifier="irrelevant", line=5
            ),
            _make_snippet(file="file2.cpp", kind=SnippetKind.Noexcept, line=10),
        ]

        non_noexcept_move_snippet = [
            _make_snippet(
                file="file3.cpp",
                kind=SnippetKind.NonNoexceptMove,
                identifier="TypeName::TypeName",
                line=15,
            ),
        ]

        # The non-noexcept move is present on both sides, no alert.
        alerts = analyze_changes(non_noexcept_move_snippet, non_noexcept_move_snippet)
        self.assertEqual(len(alerts), 0)
        alerts = analyze_changes(
            other_snippets + non_noexcept_move_snippet, other_snippets + non_noexcept_move_snippet
        )
        self.assertEqual(len(alerts), 0)

        # Other stuff is changing, but the non-noexcept move is not changing, no alert.
        alerts = analyze_changes(
            other_snippets + non_noexcept_move_snippet, non_noexcept_move_snippet
        )
        self.assertFalse(any(alert.kind == AlertKind.NonNoexceptMove for alert in alerts))
        alerts = analyze_changes(
            non_noexcept_move_snippet, other_snippets + non_noexcept_move_snippet
        )
        self.assertFalse(any(alert.kind == AlertKind.NonNoexceptMove for alert in alerts))

        # The non-noexcept move is removed, this is fine, no alert.
        alerts = analyze_changes(other_snippets + non_noexcept_move_snippet, other_snippets)
        self.assertEqual(len(alerts), 0)

        # The non-noexcept move is added, this is an alert.
        alerts = analyze_changes(other_snippets, other_snippets + non_noexcept_move_snippet)
        self.assertEqual(len(alerts), 1)
        alert = alerts[0]
        self.assertEqual(alert.file, "file3.cpp")
        self.assertEqual(alert.line, 15)
        self.assertEqual(alert.identifier, "TypeName::TypeName")
        self.assertEqual(alert.kind, AlertKind.NonNoexceptMove)

    def test_analyze_changes_complex_case(self):
        # We have a non-noexcept move (TypeName) in both pre and post, so no alert for that.
        # We have a net addition of noexcept, so alert for that.
        # We have a new non-noexcept move (OtherType), so alert for that.
        pre_snippets = [
            _make_snippet(file="file1.cpp", kind=SnippetKind.Noexcept, line=10),
            _make_snippet(
                file="file2.cpp",
                kind=SnippetKind.NonNoexceptMove,
                identifier="TypeName::TypeName",
                line=20,
            ),
        ]
        post_snippets = [
            _make_snippet(file="file1.cpp", kind=SnippetKind.Noexcept, line=10),
            _make_snippet(file="file1.cpp", kind=SnippetKind.Noexcept, line=30),
            _make_snippet(
                file="file2.cpp",
                kind=SnippetKind.NoexceptMove,
                identifier="TypeName::TypeName",
                line=20,
            ),
            _make_snippet(
                file="file3.cpp",
                kind=SnippetKind.NonNoexceptMove,
                identifier="OtherType::OtherType",
                line=30,
            ),
            _make_snippet(
                file="file4.cpp",
                kind=SnippetKind.NonNoexceptMove,
                identifier="TypeName::TypeName",
                line=40,
            ),
        ]

        alerts = analyze_changes(pre_snippets, post_snippets)
        self.assertEqual(len(alerts), 2)

        noexcept_move_alerts = [
            alert for alert in alerts if alert.kind == AlertKind.NonNoexceptMove
        ]
        self.assertEqual(len(noexcept_move_alerts), 1)
        noexcept_move_alert = noexcept_move_alerts[0]
        self.assertEqual(noexcept_move_alert.file, "file3.cpp")
        self.assertEqual(noexcept_move_alert.line, 30)
        self.assertEqual(noexcept_move_alert.identifier, "OtherType::OtherType")

        noexcept_change_alerts = [alert for alert in alerts if alert.kind == AlertKind.Noexcept]
        self.assertEqual(len(noexcept_change_alerts), 1)
        noexcept_change_alert = noexcept_change_alerts[0]
        self.assertEqual(noexcept_change_alert.file, "file1.cpp")

    def test_analyze_changes_definitive_additions(self):
        post_snippets = [
            _make_snippet(file="file1.cpp", kind=SnippetKind.Noexcept, line=10),
            _make_snippet(file="file1.cpp", kind=SnippetKind.NoexceptAddition, line=20),
            _make_snippet(file="file1.cpp", kind=SnippetKind.NoexceptAddition, line=30),
            _make_snippet(file="file1.cpp", kind=SnippetKind.Noexcept, line=40),
        ]
        alerts = analyze_changes([], post_snippets)
        # Only alert once for a definitive addition, even if there are multiple snippets.
        # Also don't alert for the net additions of noexcept since it would be repetitive.
        self.assertEqual(len(alerts), 1)
        alert = alerts[0]
        self.assertEqual(alert.file, "file1.cpp")
        self.assertEqual(alert.kind, AlertKind.Noexcept)
        self.assertEqual(alert.line, 20)

    def test_analyze_changes_definitive_removals(self):
        pre_snippets = [
            _make_snippet(file="file1.cpp", kind=SnippetKind.Noexcept, line=10, alert_line=510),
            _make_snippet(
                file="file1.cpp", kind=SnippetKind.NoexceptAddition, line=20, alert_line=520
            ),
            _make_snippet(
                file="file1.cpp", kind=SnippetKind.NoexceptAddition, line=30, alert_line=530
            ),
            _make_snippet(file="file1.cpp", kind=SnippetKind.Noexcept, line=40, alert_line=540),
        ]
        alerts = analyze_changes(pre_snippets, [])
        # Only alert once for a definitive removal, even if there are multiple snippets.
        self.assertEqual(len(alerts), 1)
        alert = alerts[0]
        self.assertEqual(alert.file, "file1.cpp")
        self.assertEqual(alert.kind, AlertKind.Noexcept)
        self.assertEqual(alert.line, 520)


if __name__ == "__main__":
    unittest.main()
