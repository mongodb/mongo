import argparse
import json
import os
import hashlib


def get_replacements_for_file(filepath: str, file_data: dict, fixes: dict):
    # we cant reliably make any changes unless we can use the md5 to make
    # sure the offsets are going to match up.
    if file_data.get('md5'):
        for byte_offset in file_data:
            if byte_offset != 'md5':
                if file_data['md5'] in fixes:
                    fixes[file_data['md5']]['replacements'].extend(file_data[byte_offset].get(
                        'replacements', []))
                else:
                    fixes[file_data['md5']] = {
                        'replacements': file_data[byte_offset].get('replacements', []),
                        'filepath': filepath,
                    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(dest="fixes_file", help="Path to fixes file.")
    args = parser.parse_args()

    fixes = {}

    # Extract just the replacement data, organizing by file
    with open(args.fixes_file) as fin:
        fixes_data = json.load(fin)
        for check in fixes_data:
            for file in fixes_data[check]:
                get_replacements_for_file(file, fixes_data[check][file], fixes)
    # This first for loop is for each file we intended to make changes to
    for recorded_md5 in fixes:
        current_md5 = None
        file_bytes = None

        if os.path.exists(fixes[recorded_md5]['filepath']):
            with open(fixes[recorded_md5]['filepath'], 'rb') as fin:
                file_bytes = fin.read()
                current_md5 = hashlib.md5(file_bytes).hexdigest()

        # if the md5 is not matching up the file must have changed and our offset may
        # not be correct, well need to bail on this file.
        if current_md5 != recorded_md5:
            print(
                f'ERROR: not applying replacements for {fixes[recorded_md5]["filepath"]}, the file has changed since the replacement offset was recorded.'
            )
            continue

        # perform the swap replacement of the binary data
        file_bytes = bytearray(file_bytes)
        for replacement in fixes[recorded_md5]['replacements']:
            file_bytes[replacement['Offset']:replacement['Offset'] +
                       replacement['Length']] = replacement['ReplacementText'].encode()

        with open(fixes[recorded_md5]['filepath'], 'wb') as fout:
            fout.write(bytes(file_bytes))


if __name__ == '__main__':
    main()
