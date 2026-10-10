#!/usr/bin/env python3
"""Generate and sign a code set for one gate.

Writes the file the device fetches. No backend required: run this, put
the result anywhere the gate can reach over HTTPS, and the whole
device-side path is exercised.

  ./make_sync_file.py --gate portao-01 --gen 7 \
      --code a1b2c3d4 123456AB# 2026-10-05T14:00 2026-10-08T11:00 \
      > gate-portao-01.txt

The hash the device stores is sha256(code || device_salt), so this
script needs the same salt the firmware uses. On a dev build that is
DEV_SALT; in production it comes off the credential dongle.

The generation must increase on every change. The device refuses
anything not strictly newer than what it holds, which is what stops a
captured file from restoring a revoked code.
"""
import argparse
import hashlib
import hmac
import sys
from datetime import datetime, timezone


def parse_when(s: str) -> int:
    """Accept an epoch, or an ISO time which is read as UTC."""
    if s.isdigit():
        return int(s)
    return int(datetime.fromisoformat(s).replace(tzinfo=timezone.utc).timestamp())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", required=True, help="device id, e.g. portao-01")
    ap.add_argument("--gen", type=int, required=True,
                    help="generation; must increase on every change")
    ap.add_argument("--salt", required=True,
                    help="device salt: must match the firmware's")
    ap.add_argument("--key", required=True,
                    help="delivery key, hex: must match the firmware's")
    ap.add_argument("--code", nargs=4, action="append", default=[],
                    metavar=("ID", "PLAINTEXT", "FROM", "UNTIL"),
                    help="repeatable; FROM/UNTIL are epoch or ISO UTC")
    args = ap.parse_args()

    lines = [f"portao-sync v1", f"gate {args.gate}", f"gen {args.gen}"]

    for code_id, plaintext, start, finish in args.code:
        if len(code_id) != 8 or any(c not in "0123456789abcdef" for c in code_id):
            print(f"error: id {code_id!r} must be exactly 8 lowercase hex characters;\n"
                  f"       the device refuses anything else, so that a delivered code\n"
                  f"       can never collide with a console-created one.",
                  file=sys.stderr)
            return 1
        digest = hashlib.sha256(plaintext.encode() + args.salt.encode()).hexdigest()
        lines.append(f"code {code_id} {digest} {parse_when(start)} {parse_when(finish)}")

    lines.append("end")
    signed = "\n".join(lines) + "\n"

    mac = hmac.new(bytes.fromhex(args.key), signed.encode(), hashlib.sha256).hexdigest()
    sys.stdout.write(signed + f"mac {mac}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
