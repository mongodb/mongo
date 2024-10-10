"""Helper tools to get OAuth credentials using the PKCE flow."""

from __future__ import annotations

from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, HTTPServer
from random import choice
from string import ascii_lowercase
from typing import Any, Callable, Optional, Tuple
from urllib.parse import parse_qs, urlsplit
from webbrowser import open as web_open

import requests
from oauthlib.oauth2 import BackendApplicationClient
from pkce import generate_pkce_pair
from pydantic import ValidationError
from pydantic.main import BaseModel
from requests_oauthlib import OAuth2Session

from buildscripts.util.fileops import read_yaml_file

AUTH_HANDLER_RESPONSE = """\
<html>
  <head>
    <title>Authentication Status</title>
    <script>
    window.onload = function() {
      window.close();
    }
    </script>
  </head>
  <body>
    <p>The authentication flow has completed.</p>
  </body>
</html>
""".encode("utf-8")


class Configs:
    """Collect configurations necessary for authentication process."""

    # pylint: disable=invalid-name

    AUTH_DOMAIN = "corp.mongodb.com/oauth2/aus4k4jv00hWjNnps297"
    CLIENT_ID = "0oa5zf9ps4N3JKWIJ297"
    REDIRECT_PORT = 8989
    SCOPE = "kanopy+openid+profile"

    def __init__(
        self,
        client_credentials_scope: str = None,
        client_credentials_user_name: str = None,
        auth_domain: str = None,
        client_id: str = None,
        redirect_port: int = None,
        scope: str = None,
    ):
        """Initialize configs instance."""

        self.AUTH_DOMAIN = auth_domain or self.AUTH_DOMAIN
        self.CLIENT_ID = client_id or self.CLIENT_ID
        self.REDIRECT_PORT = redirect_port or self.REDIRECT_PORT
        self.SCOPE = scope or self.SCOPE
        self.CLIENT_CREDENTIALS_SCOPE = client_credentials_scope
        self.CLIENT_CREDENTIALS_USER_NAME = client_credentials_user_name


class OAuthCredentials(BaseModel):
    """OAuth access token and its associated metadata."""

    expires_in: int
    access_token: str
    created_time: datetime
    user_name: str

    def are_expired(self) -> bool:
        """
        Check whether the current OAuth credentials are expired or not.

        :return: Whether the credentials are expired or not.
        """
        return self.created_time + timedelta(seconds=self.expires_in) < datetime.now()

    @classmethod
    def get_existing_credentials_from_file(cls, file_path: str) -> Optional[OAuthCredentials]:
        """
        Try to get OAuth credentials from a file location.

        Will return None if credentials either don't exist or are expired.
        :param file_path: Location to check for OAuth credentials.
        :return: Valid OAuth credentials or None if valid credentials don't exist
        """
        try:
            creds = OAuthCredentials(**read_yaml_file(file_path))
            if (
                creds.access_token
                and creds.created_time
                and creds.expires_in
                and creds.user_name
                and not creds.are_expired()
            ):
                return creds
            else:
                return None
        except ValidationError:
            return None
        except OSError:
            return None


class _RedirectServer(HTTPServer):
    """HTTP server to use when fetching OAuth credentials using the PKCE flow."""

    pkce_credentials: Optional[OAuthCredentials] = None
    auth_domain: str
    client_id: str
    redirect_uri: str
    code_verifier: str

    def __init__(
        self,
        server_address: Tuple[str, int],
        handler: Callable[..., BaseHTTPRequestHandler],
        redirect_uri: str,
        auth_domain: str,
        client_id: str,
        code_verifier: str,
    ):
        self.redirect_uri = redirect_uri
        self.auth_domain = auth_domain
        self.client_id = client_id
        self.code_verifier = code_verifier
        super().__init__(server_address, handler)


class _Handler(BaseHTTPRequestHandler):
    """Request handler class to use when trying to get OAuth credentials."""

    # pylint: disable=invalid-name

    server: _RedirectServer

    def _set_response(self) -> None:
        """Set the response to the server making a request."""
        self.send_response(200)
        self.send_header("Content-type", "text/html")
        self.end_headers()

    def log_message(self, log_format: Any, *args: Any) -> None:  # pylint: disable=unused-argument,arguments-differ
        """
        Log HTTP Server internal messages.

        :param log_format: The format to use when logging messages.
        :param args: Key word args.
        """
        return None

    def do_GET(self) -> None:
        """Handle the callback response from the auth server."""
        params = parse_qs(urlsplit(self.path).query)
        code = params.get("code")

        if not code:
            raise ValueError("Could not get authorization code when signing in to Okta")

        url = f"https://{self.server.auth_domain}/v1/token"
        body = {
            "grant_type": "authorization_code",
            "client_id": self.server.client_id,
            "code_verifier": self.server.code_verifier,
            "code": code,
            "redirect_uri": self.server.redirect_uri,
        }

        resp = requests.post(url, data=body).json()

        access_token = resp.get("access_token")
        expires_in = resp.get("expires_in")

        if not access_token or not expires_in:
            raise ValueError("Could not get access token or expires_in data about access token")

        headers = {"Authorization": f"Bearer {access_token}"}
        resp = requests.get(
            f"https://{self.server.auth_domain}/v1/userinfo", headers=headers
        ).json()

        split_username = resp["preferred_username"].split("@")

        if len(split_username) != 2:
            raise ValueError("Could not get user_name of current user")

        self.server.pkce_credentials = OAuthCredentials(
            access_token=access_token,
            expires_in=expires_in,
            created_time=datetime.now(),
            user_name=split_username[0],
        )
        self._set_response()
        self.wfile.write(AUTH_HANDLER_RESPONSE)


class PKCEOauthTools:
    """Basic toolset to get OAuth credentials using the PKCE flow."""

    auth_domain: str
    client_id: str
    redirect_port: int
    redirect_uri: str
    scope: str

    def __init__(self, auth_domain: str, client_id: str, redirect_port: int, scope: str):
        """
        Create a new PKCEOauth tools instance.

        :param auth_domain: The uri of the auth server to get the credentials from.
        :param client_id: The id of the client that you are using to authenticate.
        :param redirect_port: Port to use when setting up the local server for the auth redirect.
        :param scope: The OAuth scopes to request access for.
        """
        self.auth_domain = auth_domain
        self.client_id = client_id
        self.redirect_port = redirect_port
        self.redirect_uri = f"http://localhost:{redirect_port}/"
        self.scope = scope

    def get_pkce_credentials(self, print_auth_url: bool = False) -> OAuthCredentials:
        """
        Try to get an OAuth access token and its associated metadata.

        :param print_auth_url: Whether to print the auth url to the console instead of opening it.
        :return: OAuth credentials and some associated metadata to check if they have expired.
        """
        code_verifier, code_challenge = generate_pkce_pair()

        state = "".join(choice(ascii_lowercase) for i in range(10))

        authorization_url = (
            f"https://{self.auth_domain}/v1/authorize?"
            f"scope={self.scope}&"
            f"response_type=code&"
            f"response_mode=query&"
            f"client_id={self.client_id}&"
            f"code_challenge={code_challenge}&"
            f"state={state}&"
            f"code_challenge_method=S256&"
            f"redirect_uri={self.redirect_uri}"
        )

        httpd = _RedirectServer(
            ("", self.redirect_port),
            _Handler,
            self.redirect_uri,
            self.auth_domain,
            self.client_id,
            code_verifier,
        )
        if print_auth_url:
            print("Please open the below url in a browser and sign in if necessary")
            print(authorization_url)
        else:
            web_open(authorization_url)
        httpd.handle_request()

        if not httpd.pkce_credentials:
            raise ValueError(
                "Could not retrieve Okta credentials to talk to Kanopy with. "
                "Please sign out of Okta in your browser and try runnning this script again"
            )

        return httpd.pkce_credentials


def get_oauth_credentials(configs: Configs, print_auth_url: bool = False) -> OAuthCredentials:
    """
    Run the OAuth workflow to get credentials for a human user.

    :param configs: Configs instance.
    :param print_auth_url: Whether to print the auth url to the console instead of opening it.
    :return: OAuth credentials for the given user.
    """
    oauth_tools = PKCEOauthTools(
        auth_domain=configs.AUTH_DOMAIN,
        client_id=configs.CLIENT_ID,
        redirect_port=configs.REDIRECT_PORT,
        scope=configs.SCOPE,
    )
    credentials = oauth_tools.get_pkce_credentials(print_auth_url)
    return credentials


def get_client_cred_oauth_credentials(
    client_id: str, client_secret: str, configs: Configs
) -> OAuthCredentials:
    """
    Run the OAuth workflow to get credentials for a machine user.

    :param client_id: The client_id of the machine user to authenticate as.
    :param client_secret: The client_secret of the machine user to authenticate as.
    :param configs: Configs instance.
    :return: OAuth credentials for the given machine user.
    """
    client = BackendApplicationClient(client_id=client_id)
    oauth = OAuth2Session(client=client)
    token = oauth.fetch_token(
        token_url=f"https://{configs.AUTH_DOMAIN}/v1/token",
        client_id=client_id,
        client_secret=client_secret,
        scope=configs.CLIENT_CREDENTIALS_SCOPE,
    )
    access_token = token.get("access_token")
    expires_in = token.get("expires_in")

    if not access_token or not expires_in:
        raise ValueError("Could not get access token or expires_in data about access token")

    return OAuthCredentials(
        access_token=access_token,
        expires_in=expires_in,
        created_time=datetime.now(),
        user_name=configs.CLIENT_CREDENTIALS_USER_NAME,
    )
