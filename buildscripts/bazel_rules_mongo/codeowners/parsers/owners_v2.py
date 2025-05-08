import pathlib

from codeowners.parsers.owners_v1 import OwnersParserV1


# Parser for OWNERS.yml files version 2.0.0
class OwnersParserV2(OwnersParserV1):
    def get_owner_line(self, directory: str, pattern: str, owners: set[str]) -> str:
        # ensure the path is correct and consistent on all platforms
        directory = pathlib.PurePath(directory).as_posix()

        if directory == ".":
            # we are in the root dir and can directly pass the pattern
            parsed_pattern = pattern
        elif not pattern or pattern == "*":
            # If there is no pattern or a wildcard add the directory as the pattern.
            parsed_pattern = f"/{directory}/"
        else:
            # if the pattern contains a slash the pattern should be treated as relative to the
            # directory it came from.
            if pattern.startswith("/"):
                parsed_pattern = f"/{directory}{pattern}"
            else:
                parsed_pattern = f"/{directory}/{pattern}"

        if not self.test_pattern(parsed_pattern):
            raise (RuntimeError(f"Can not find any files that match pattern: `{pattern}`"))

        return self.get_line(parsed_pattern, owners)
