"""
Common routines and data for clang tools
"""

clang_win32_dirs = [
    r'C:\Program Files\LLVM\bin',
    r'C:\cygwin64\bin',
    r'C:\msys64',
    r'C:\msys64\mingw64\bin',
    r'C:\cygwin\bin',
    r'C:\msys',
]

def get_clang_install_dirs(platform):
    if platform == 'win32':
        return clang_win32_dirs
    else:
        return []