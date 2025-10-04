#!/usr/bin/env python3

import os
import sys

FILE_SIZE_THRESHOLD_IN_BYTES = 16 * 1024 * 1024  # 16MB


def main():
    args = sys.argv[1:]
    file_name = args[0]
    file_path = os.path.join(os.path.dirname(os.getcwd()), file_name)
    if os.path.exists(file_path):
        file_size_in_bytes = os.path.getsize(file_path)
        if file_size_in_bytes > FILE_SIZE_THRESHOLD_IN_BYTES:
            print(
                f"WARNING! {file_name} is {file_size_in_bytes} bytes, exceeding threshold"
                f" {FILE_SIZE_THRESHOLD_IN_BYTES} bytes, file upload may fail due to network issues, or Evergreen"
                f" may reject very large yaml sizes"
            )
        else:
            print(
                f"{file_name} is {file_size_in_bytes} bytes, below threshold {FILE_SIZE_THRESHOLD_IN_BYTES} bytes"
            )
    else:
        print(f"{file_path} does not exist")


if __name__ == "__main__":
    main()
