import os
import sys
from pathlib import Path

import typer
from google.oauth2.credentials import Credentials
from googleapiclient.discovery import build
from googleapiclient.errors import HttpError
from googleapiclient.http import MediaFileUpload

app = typer.Typer(add_completion=False)


@app.command()
def upload(
    input_file: Path = typer.Argument(
        ...,
        exists=True,
        file_okay=True,
        dir_okay=False,
        readable=True,
        resolve_path=True,
        help="The path to the file to upload.",
    ),
    client_id: str = typer.Option(
        ...,
        envvar="SAST_REPORT_UPLOAD_GOOGLE_CLIENT_ID",
        help="The OAuth2 client ID for Google API.",
    ),
    client_secret: str = typer.Option(
        ...,
        envvar="SAST_REPORT_UPLOAD_GOOGLE_CLIENT_SECRET",
        help="The OAuth2 client secret for Google API.",
    ),
    refresh_token: str = typer.Option(
        ...,
        envvar="SAST_REPORT_UPLOAD_GOOGLE_CLIENT_REFRESH_TOKEN",
        help="The OAuth2 refresh token for Google API.",
    ),
    branch: str = typer.Option(
        ..., envvar="MONGODB_RELEASE_BRANCH", help="The MongoDB release branch."
    ),
    test_folder_id: str = typer.Option(
        ...,
        envvar="SBOM_REPORT_TEST_GOOGLE_DRIVE_FOLDER_ID",
        help="The ID of the Google Drive folder for test uploads.",
    ),
    releases_folder_id: str = typer.Option(
        ...,
        envvar="SBOM_REPORT_RELEASES_GOOGLE_DRIVE_FOLDER_ID",
        help="The ID of the Google Drive folder for releases.",
    ),
    triggered_by_tag: str = typer.Option(
        "",
        envvar="TRIGGERED_BY_GIT_TAG",
        help="Indicates if the upload was triggered by a git tag.",
    ),
    version: str = typer.Option(
        "", envvar="MONGODB_VERSION", help="The version of MongoDB being processed."
    ),
    upload_file_name: str = typer.Option(
        None,
        envvar="UPLOAD_FILE_NAME",
        help="Optional upload file to use. If not provided, the input file name will be used.",
    ),
):
    print("Starting file upload process to Google Drive.")

    try:
        creds_info = {
            "client_id": client_id,
            "client_secret": client_secret,
            "refresh_token": refresh_token,
            "token_uri": "https://oauth2.googleapis.com/token",
        }
        creds = Credentials.from_authorized_user_info(
            info=creds_info, scopes=["https://www.googleapis.com/auth/drive"]
        )
        drive_service = build("drive", "v3", credentials=creds)
        print("Authenticated with Google Drive API successfully.")
    except Exception as e:
        print(f"Failed to authenticate with Google API: {e}")
        sys.exit(1)

    folder_id = releases_folder_id if triggered_by_tag.lower() == "true" else test_folder_id

    if upload_file_name is None:
        input_file_name_str = str(input_file.resolve().name)
    else:
        _, file_extension = os.path.splitext(input_file.resolve().name)
        input_file_name_str = f"{upload_file_name}{file_extension}"

    file_metadata = {"name": input_file_name_str, "parents": [folder_id]}
    media = MediaFileUpload(str(input_file), mimetype="text/html", resumable=True)

    try:
        print(f"Uploading '{input_file}' to Google Drive...")
        print(f"File name on Drive: '{input_file_name_str}'")
        file = (
            drive_service.files()
            .create(
                body=file_metadata,
                media_body=media,
                fields="id, webViewLink",
                supportsAllDrives=True,
            )
            .execute()
        )
        print("Upload complete.")
        print(f"File ID: {file.get('id')}")
        print(f"View Link: {file.get('webViewLink')}")

    except HttpError as error:
        print(f"An API error occurred: {error}")
        sys.exit(1)


if __name__ == "__main__":
    app()
