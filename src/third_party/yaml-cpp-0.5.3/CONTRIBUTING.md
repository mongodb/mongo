# Style

This project is formatted with [clang-format][fmt] using the style file at the root of the repository. Please run clang-format before sending a pull request.

In general, try to follow the style of surrounding code.

[fmt]: http://clang.llvm.org/docs/ClangFormat.html

# Tests

Please verify the tests pass by running the target `tests/run_tests`.

If you are adding functionality, add tests accordingly.

# Pull request process

Every pull request undergoes a code review. Unfortunately, github's code review process isn't great, but we'll manage. During the code review, if you make changes, add new commits to the pull request for each change. Once the code review is complete, rebase against the master branch and squash into a single commit.
