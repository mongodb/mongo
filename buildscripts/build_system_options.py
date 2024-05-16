"""Options used for building process."""

import re


class PathOptions:
    """Get options/methods to resolve path-related stuff."""

    main_binary_folder_name = "bin"
    shared_library_folder_name = "lib"

    # extend the below list if there are new types of shared libraries
    _shared_library_file_patterns = [r".+\.so(\.(\d{1,}|debug))?$", r".+\.dylib$"]
    _compiled_shared_library_file_patterns = None

    @property
    def shared_library_file_patterns(self) -> list:
        """Cache & return compiled regex patterns for further usage."""

        if not self._compiled_shared_library_file_patterns:
            self._compiled_shared_library_file_patterns = tuple(
                map(re.compile, self._shared_library_file_patterns)
            )
        return self._compiled_shared_library_file_patterns

    def is_shared_library_file(self, path: str) -> bool:
        """Check if given path points to a shared library file."""

        for pattern in self.shared_library_file_patterns:
            if pattern.match(path):
                return True
        return False

    def get_binary_folder_name(self, path: str) -> str:
        """Analyze path/file and return name of folder which contains that file."""

        if self.is_shared_library_file(path):
            return self.shared_library_folder_name
        # fallback to default, main binary folder
        return self.main_binary_folder_name
