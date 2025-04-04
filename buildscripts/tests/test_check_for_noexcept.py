import unittest

from buildscripts.check_for_noexcept import (
    ChangeKind,
    analyze_text_diff,
    get_matching_blocks,
    get_nonmatching_blocks,
)


def _remove_whitespace(s: str) -> str:
    return "".join(s.split())


class TestGetMatchingBlocks(unittest.TestCase):
    def _check_blocks_cover_string(self, blocks: list[tuple[int, int]], s: str):
        if len(blocks) == 0:
            self.assertEqual(len(s), 0)
            return

        prev_end = 0
        for block_start, block_end in sorted(blocks):
            self.assertEqual(block_start, prev_end)
            prev_end = block_end
        self.assertEqual(prev_end, len(s))

    def test_matches_and_nonmatches_are_accurate(self):
        cases = (
            ("const noexcept try {", "const try {"),
            ("a b c d", "a b c d e"),
            ("b c d e", "a b c d e"),
            ("int f(SomeType<float> x) noexcept const {", "int f(SomeType<float> x) const {"),
            ("int f(SomeType<float> x) noexcept const\n{", "int\nf(SomeType<float> x) const {"),
        )
        for lhs, rhs in cases:
            matching = get_matching_blocks(lhs, rhs)
            nonmatching = get_nonmatching_blocks(lhs, rhs)
            self.assertGreater(len(matching), 0)
            for lhs_start, lhs_end, rhs_start, rhs_end in matching:
                block_lhs = lhs[lhs_start:lhs_end]
                block_rhs = rhs[rhs_start:rhs_end]
                # Note that the matching ignores whitespace, so "matching" sequences may
                # differ in whitespace.
                self.assertEqual(_remove_whitespace(block_lhs), _remove_whitespace(block_rhs))
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

    def test_no_match(self):
        self.assertEqual(get_matching_blocks("a b c d", "e f g h"), [])

    def test_equal_strings(self):
        s = "a b c d"
        self.assertEqual(get_matching_blocks(s, s), [(0, len(s), 0, len(s))])


class TestCheckForNoexcept(unittest.TestCase):
    def test_no_text(self):
        changes = analyze_text_diff("", "")
        self.assertEqual(changes, [])

    def test_simple_additions(self):
        pre = "int f(SomeType<float> x) const {"
        posts = (
            "int f(SomeType<float> x) noexcept const {",
            "int f(SomeType<float> x) noexcept {",
            "int func(SomeType<float> x) noexcept const {",
            "int f(SomeType<double> x) noexcept const {",
            "int f(SomeType<float> y) noexcept const {",
            "int f(SomeOtherType<float> x) noexcept const {",
        )
        for post in posts:
            changes = analyze_text_diff(pre, post)
            self.assertEqual(len(changes), 1)
            change = changes[0]
            self.assertEqual(change.kind, ChangeKind.ADDITION)
            self.assertTrue(change.is_certain)
            self.assertEqual(change.index, post.find("noexcept"))
            self.assertEqual(change.line, 1)

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
            changes = analyze_text_diff(pre, post)
            self.assertEqual(len(changes), 1)
            change = changes[0]
            self.assertEqual(change.kind, ChangeKind.REMOVAL)
            self.assertTrue(change.is_certain)
            self.assertEqual(change.index, pre.find("noexcept"))
            self.assertEqual(change.line, 1)

    def test_simple_unchanged(self):
        pre = "int f(SomeType<float> x) noexcept {"
        post = "int f(SomeType<float> x) noexcept {"
        changes = analyze_text_diff(pre, post)
        self.assertEqual(changes, [])

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
            changes = analyze_text_diff(pre, post)
            self.assertEqual(changes, [])

    def test_complex_change(self):
        pre = """
class SomeClass {
    template<typename T>
    auto memberFunction(std::unique_ptr<T> x) const {
        thisFunctionCanThrow();
        return x.get();
    }
    void otherMemberFunction() const noexcept try {
        thisFunctionCanThrow()
    } catch (DBException& e) {
    }
};
"""
        post = """
class SomeClass {
    template<typename T>
    int memberFunctionNoThrow(std::unique_ptr<T> x) const noexcept {
        return x.get();
    }
    void otherMemberFunction() const {
        thisFunctionCanThrow()
    }
};
"""
        changes = analyze_text_diff(pre, post)
        additions = [change for change in changes if change.kind == ChangeKind.ADDITION]
        removals = [change for change in changes if change.kind == ChangeKind.REMOVAL]
        self.assertEqual(len(additions), 1)
        self.assertEqual(len(removals), 1)
        addition = additions[0]
        removal = removals[0]
        # Note that lines are all relative to the "post" side and the "SomeClass" is line 2
        # (lines are 1-indexed and line 1 is a blank line).
        self.assertEqual(addition.line, 4)
        self.assertEqual(removal.line, 7)

    def test_rewrite_change(self):
        pre = """
class SomeClass {
    auto memberFunction(std::unique_ptr<int> x) const { return x.get(); }
    void otherMemberFunction() const { return 7; }
    template<typename T>
    T anotherMemberFunction(T x) const { return x; }
};
"""
        post = """
class SomeClass {
    template<typename T>
    void anotherNewNoexceptFunction() noexcept { doSomething(); }
    auto someNewNoexceptFunction(std::shared_ptr<T> x) const noexcept { return *x; }
    int anotherNewNoexceptFunction2() const noexcept { return 7; }
};
"""
        changes = analyze_text_diff(pre, post)
        additions = [change for change in changes if change.kind == ChangeKind.ADDITION]
        removals = [change for change in changes if change.kind == ChangeKind.REMOVAL]
        self.assertEqual(len(additions), 3)
        self.assertEqual(len(removals), 0)
        lines = sorted([addition.line for addition in additions])
        self.assertEqual(lines, [4, 5, 6])


if __name__ == "__main__":
    unittest.main()
