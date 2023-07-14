import argparse
import subprocess
import sys


def get_python_version_string(python_path:str):
    output = subprocess.check_output([python_path, '--version'], stderr=subprocess.STDOUT, text=True)
    version_string = output.strip('\n')
    return version_string


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--cmake', help='Path to the CMakeCache.txt file')
    parser.add_argument('-s',
                        '--search_string',
                        help='Search string to find the Python config in the CMakeCache.txt file')
    parser.add_argument('-v', '--verbose', action="store_true", help='be verbose')
    args = parser.parse_args()

    if args.verbose:
        print('Python Version Analyser')
        print('=======================')
        print("This tool confirms that the Python version being used matches that configured in CMake.")
        print('Configuration:')
        print('  CMakeCache.txt file:      {}'.format(args.cmake))
        print('  Config search string:     {}'.format(args.search_string))

    this_path = sys.executable
    this_version = get_python_version_string(this_path)

    cmakecache_txt_path = args.cmake
    config_search_string = args.search_string

    cmake_python_path = None
    cmake_python_version = None

    if cmakecache_txt_path is not None and config_search_string is not None:
        with open(cmakecache_txt_path, 'r') as file:
            lines = file.readlines()
            for line in lines:
                # Select the first line that matches.
                if line.find(config_search_string) != -1:
                    line = line.strip('\n')
                    cmake_python_path = '/' + line.split('/', 1)[1]
                    break

    if cmake_python_path is not None:
        cmake_python_version = get_python_version_string(cmake_python_path)

    if args.verbose:
        print("Results:")
        print("  This Python:")
        print("      path  = {}".format(this_path))
        print("      version  = {}".format(this_version))
        print("  CMake Python")
        print("      path  = {}".format(cmake_python_path))
        print("      version  = {}".format(cmake_python_version))

    if this_version == cmake_python_version:
        print("Python versions ({} and {}) are the same".format(this_version, cmake_python_version))
    else:
        print("==== Python versions are DIFFERENT ===")
        sys.exit("Error: Python versions ({} and {}) are DIFFERENT!".format(this_version, cmake_python_version))


if __name__ == '__main__':
    main()
