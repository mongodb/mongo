This directory contains the ASIO C++ Library.

[scripts/import.sh][1] clones files from [mongodb-forks/asio][2] into [dist/][6].

[mongodb-forks/asio][2] is a fork of [chriskohlhoff/asio][3]. See [import.sh][1] for the
particular branch cloned.

[test/][4] contains [mongo/unittest][5]-style unit tests that verify the behavior of
MongoDB-specific modifications made to the asio fork.

When making modifications to [dist/][6], before committing here, first commit to the appropriate
branch in [mongodb-forks/asio][2]. Then `rm -r dist/` and `scripts/import.sh` to ensure that
what is here is consistent with the fork.

[1]: scripts/import.sh
[2]: https://github.com/mongodb-forks/asio
[3]: https://github.com/chriskohlhoff/asio
[4]: test/
[5]: ../../mongo/unittest/unittest.h
[6]: dist/
