"""Script to fix up our MSI files."""

import argparse
import msilib
import shutil


def exec_delete(db, query):
    """Execute delete on db."""
    view = db.OpenView(query)
    view.Execute(None)

    cur_record = view.Fetch()
    view.Modify(msilib.MSIMODIFY_DELETE, cur_record)
    view.Close()


def exec_update(db, query, column, value):
    """Execute update on db."""
    view = db.OpenView(query)
    view.Execute(None)

    cur_record = view.Fetch()
    cur_record.SetString(column, value)
    view.Modify(msilib.MSIMODIFY_REPLACE, cur_record)
    view.Close()


def main():
    """Execute Main program."""
    parser = argparse.ArgumentParser(description="Trim MSI.")
    parser.add_argument("file", type=argparse.FileType("r"), help="file to trim")
    parser.add_argument("out", type=argparse.FileType("w"), help="file to output to")

    args = parser.parse_args()
    print("Trimming MSI")

    db = msilib.OpenDatabase(args.file.name, msilib.MSIDBOPEN_DIRECT)

    exec_delete(
        db,
        "select * from ControlEvent WHERE Dialog_ = 'LicenseAgreementDlg' AND Control_ = 'Next' AND Event = 'NewDialog' AND Argument = 'CustomizeDlg'",
    )
    exec_delete(
        db,
        "select * from ControlEvent WHERE Dialog_ = 'CustomizeDlg' AND Control_ = 'Back' AND Event = 'NewDialog' AND Argument = 'LicenseAgreementDlg'",
    )
    exec_delete(
        db,
        "select * from ControlEvent WHERE Dialog_ = 'CustomizeDlg' AND Control_ = 'Next' AND Event = 'NewDialog' AND Argument = 'VerifyReadyDlg'",
    )
    exec_delete(
        db,
        "select * from ControlEvent WHERE Dialog_ = 'VerifyReadyDlg' AND Control_ = 'Back' AND Event = 'NewDialog' AND Argument = 'CustomizeDlg'",
    )

    db.Commit()

    shutil.copyfile(args.file.name, args.out.name)


if __name__ == "__main__":
    main()
