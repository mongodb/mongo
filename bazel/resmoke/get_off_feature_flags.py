import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

from buildscripts.idl.gen_all_feature_flag_list import get_all_feature_flags_turned_off_by_default


def main():
    off_ff = get_all_feature_flags_turned_off_by_default()
    print(" ".join(off_ff))


if __name__ == "__main__":
    main()
