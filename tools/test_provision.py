#!/usr/bin/env python3
import importlib.util
import json
import subprocess
import sys
import unittest
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("provision", HERE / "provision.py")


class ProvisionTest(unittest.TestCase):
    def test_help_lists_setup_steps(self):
        out = subprocess.run(
            [sys.executable, str(HERE / "provision.py"), "--help"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.lower()
        for word in ("cloud console", "oauth client", "auth url", "paste"):
            self.assertIn(word, out)

    def test_exchange_parses_refresh_token(self):
        provision = importlib.util.module_from_spec(SPEC)
        SPEC.loader.exec_module(provision)

        seen = {}

        class FakeResponse:
            def read(self):
                return json.dumps(
                    {"access_token": "a", "refresh_token": "r", "expires_in": 3600}
                ).encode()

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

        def fake_urlopen(req, timeout=30):
            seen["url"] = req.full_url
            seen["body"] = req.data.decode()
            return FakeResponse()

        original = urllib.request.urlopen
        urllib.request.urlopen = fake_urlopen
        try:
            token = provision.exchange_code("cid", "csecret", "code123")
        finally:
            urllib.request.urlopen = original

        self.assertEqual(token, "r")
        self.assertIn("oauth2.googleapis.com/token", seen["url"])
        self.assertIn("grant_type=authorization_code", seen["body"])
        self.assertIn("code=code123", seen["body"])

    def test_config_block_has_four_fields(self):
        provision = importlib.util.module_from_spec(SPEC)
        SPEC.loader.exec_module(provision)
        block = provision.config_json("id", "secret", "refresh", "folder")
        cfg = json.loads(block)
        self.assertEqual(
            sorted(cfg), ["client_id", "client_secret", "folder_id", "refresh_token"]
        )
        self.assertEqual(cfg["refresh_token"], "refresh")


if __name__ == "__main__":
    unittest.main(verbosity=2)
