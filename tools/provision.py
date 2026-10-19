#!/usr/bin/env python3
"""One-time Google OAuth provisioning for ps4-savedrive.

Prints config.json for the user to FTP to /data/savedrive/config.json.
Never writes the token to disk on this machine.
"""
import json
import sys
import urllib.parse
import urllib.request

AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
REDIRECT_URI = "http://localhost"
SCOPE = "https://www.googleapis.com/auth/drive"

HELP = """\
ps4-savedrive provisioning

1. Cloud Console: create or pick a Google Cloud project.
2. Enable the Google Drive API, then create an OAuth client of type
   "Desktop app". Keep the client id and client secret.
3. This script prints an auth URL. Open it, approve the drive scope, and
   copy the code parameter from the browser address bar (the page itself
   will fail to load, that is expected).
4. Paste the code here. The script prints the config JSON block; FTP it to
   /data/savedrive/config.json.

Usage: python3 provision.py [--client-id ID --client-secret SECRET --folder-id ID]
"""


def auth_url(client_id):
    query = urllib.parse.urlencode(
        {
            "client_id": client_id,
            "redirect_uri": REDIRECT_URI,
            "response_type": "code",
            "scope": SCOPE,
            "access_type": "offline",
            "prompt": "consent",
        }
    )
    return f"{AUTH_URL}?{query}"


def exchange_code(client_id, client_secret, code):
    body = urllib.parse.urlencode(
        {
            "client_id": client_id,
            "client_secret": client_secret,
            "code": code,
            "grant_type": "authorization_code",
            "redirect_uri": REDIRECT_URI,
        }
    ).encode()
    req = urllib.request.Request(
        TOKEN_URL,
        data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        payload = json.loads(resp.read())
    token = payload.get("refresh_token")
    if not token:
        raise SystemExit("no refresh_token in response (was consent already granted?)")
    return token


def config_json(client_id, client_secret, refresh_token, folder_id):
    return json.dumps(
        {
            "client_id": client_id,
            "client_secret": client_secret,
            "refresh_token": refresh_token,
            "folder_id": folder_id,
        },
        indent=2,
    )


def arg(name):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return None


def main():
    if "-h" in sys.argv or "--help" in sys.argv:
        print(HELP)
        return

    client_id = arg("--client-id") or input("OAuth client id: ").strip()
    client_secret = arg("--client-secret") or input("OAuth client secret: ").strip()
    folder_id = arg("--folder-id") or input("Drive folder id: ").strip()

    print("\nOpen this URL and approve access:\n")
    print(auth_url(client_id))
    print()

    pasted = input("Paste the code (or the full redirect URL): ").strip()
    if "code=" in pasted:
        pasted = urllib.parse.parse_qs(urllib.parse.urlparse(pasted).query)["code"][0]

    token = exchange_code(client_id, client_secret, pasted)
    print("\nSave this as /data/savedrive/config.json:\n")
    print(config_json(client_id, client_secret, token, folder_id))


if __name__ == "__main__":
    main()
