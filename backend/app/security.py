"""Security helpers used by the MVP.

The MVP uses PBKDF2 from Python's standard library to avoid external bcrypt
compatibility issues while still storing salted password hashes.
"""

from __future__ import annotations

import hashlib
import hmac
import os

PBKDF2_ALGORITHM = "sha256"
PBKDF2_ITERATIONS = 260_000
SALT_SIZE = 16


def hash_password(password: str) -> str:
    """Create a salted password hash."""
    salt = os.urandom(SALT_SIZE)
    digest = hashlib.pbkdf2_hmac(
        PBKDF2_ALGORITHM,
        password.encode("utf-8"),
        salt,
        PBKDF2_ITERATIONS,
    )
    return f"pbkdf2_{PBKDF2_ALGORITHM}${PBKDF2_ITERATIONS}${salt.hex()}${digest.hex()}"


def verify_password(password: str, password_hash: str) -> bool:
    """Verify a password against a stored PBKDF2 hash."""
    try:
        algorithm_name, iterations_text, salt_hex, digest_hex = password_hash.split("$", 3)
        if algorithm_name != f"pbkdf2_{PBKDF2_ALGORITHM}":
            return False
        iterations = int(iterations_text)
        salt = bytes.fromhex(salt_hex)
        expected_digest = bytes.fromhex(digest_hex)
    except (ValueError, TypeError):
        return False

    actual_digest = hashlib.pbkdf2_hmac(
        PBKDF2_ALGORITHM,
        password.encode("utf-8"),
        salt,
        iterations,
    )
    return hmac.compare_digest(actual_digest, expected_digest)
