#!/usr/bin/env python3
"""Generate and sign one Telegram courier line for a gate.

Produces the line you paste into the bot's chat. No backend required:
run this, post the output, and the whole device-side push path is
exercised.

  ./make_tg_line.py --counter 1 --salt "$DEVICE_SALT" --key "$DELIVERY_KEY" \\
      --add a1b2c3d4 123456AB# 2026-10-05T14:00 2026-10-08T11:00

  ./make_tg_line.py --counter 2 --salt "$DEVICE_SALT" --key "$DELIVERY_KEY" \\
      --revoke a1b2c3d4

The format, which tg_proto.c parses:

  v1|<counter>|<op>|<id>|<hash_hex>|<from>|<until>|<mac_hex>

The MAC covers everything before the final '|' - the separator itself is
not signed. Get that boundary wrong by one byte and every message is
refused as a forgery, which looks like a transport fault and is not one.

The counter must be strictly greater than the one the device holds, or
the message is refused as a replay. That is deliberate: without it,
anyone who kept a copy of an "add" could undo a revoke by posting it
again. `tg` on the dev console prints the counter currently held.
Unix epoch seconds make a good counter - monotonic, and no bookkeeping.

The hash is sha256(code || device_salt), the same construction
make_sync_file.py uses, so this script needs the same salt the firmware
has. A mismatch signs perfectly, installs the code, and then refuses it
at the keypad.

This channel is an optimisation (ADR 0007). A code pushed here that the
backend does not also know about disappears at the next fifteen-minute
poll, so push only what the backend has already committed.
"""
import argparse
import hashlib
import hmac
import sys
from datetime import datetime, timezone

# tg_proto.h: TG_LINE_MAX. The device refuses anything longer.
TG_LINE_MAX = 256

HEX = "0123456789abcdef"

# Fields a revoke does not use. They still exist, and are still signed,
# so that every message has one shape and one signed layout; tg_proto
# never reads them for a revoke.
UNUSED = "-"


def parse_when(s: str) -> int:
    """Accept an epoch, or an ISO time which is read as UTC."""
    if s.lstrip("-").isdigit():
        return int(s)
    return int(datetime.fromisoformat(s).replace(tzinfo=timezone.utc).timestamp())


def check_id(code_id: str) -> None:
    if len(code_id) != 8 or any(c not in HEX for c in code_id):
        raise SystemExit(
            f"error: id {code_id!r} must be exactly 8 lowercase hex characters;\n"
            f"       the device refuses anything else, so that a delivered code\n"
            f"       can never collide with a console-created one."
        )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--counter", type=int, required=True,
                    help="must be strictly greater than the one the device holds; "
                         "`tg` on the console prints it")
    ap.add_argument("--salt", required=True,
                    help="device salt: must match the firmware's")
    ap.add_argument("--key", required=True,
                    help="delivery key, hex: the same one make_sync_file.py uses")

    what = ap.add_mutually_exclusive_group(required=True)
    what.add_argument("--add", nargs=4,
                      metavar=("ID", "PLAINTEXT", "FROM", "UNTIL"),
                      help="FROM/UNTIL are epoch or ISO UTC")
    what.add_argument("--revoke", metavar="ID",
                      help="remove this code; the id is all a revoke carries")

    args = ap.parse_args()

    if args.counter < 0:
        raise SystemExit("error: --counter must not be negative")
    if len(args.key) != 64 or any(c not in HEX for c in args.key):
        raise SystemExit("error: --key must be 64 lowercase hex characters")
    if not args.salt:
        raise SystemExit("error: --salt must not be empty")

    if args.add:
        code_id, plaintext, start, finish = args.add
        check_id(code_id)
        valid_from = parse_when(start)
        valid_until = parse_when(finish)
        if valid_until < valid_from:
            raise SystemExit(
                f"error: the window ends before it starts "
                f"({valid_from} .. {valid_until}); the device refuses it"
            )
        digest = hashlib.sha256(plaintext.encode() + args.salt.encode()).hexdigest()
        fields = ["v1", str(args.counter), "add", code_id, digest,
                  str(valid_from), str(valid_until)]
    else:
        check_id(args.revoke)
        fields = ["v1", str(args.counter), "rev", args.revoke,
                  UNUSED, UNUSED, UNUSED]

    # Everything before the final separator. tg_proto.c computes this as
    # (field[7] - line) - 1, which is exactly the join below: the '|'
    # that precedes the MAC is NOT part of the signed region.
    signed = "|".join(fields)

    mac = hmac.new(bytes.fromhex(args.key), signed.encode(), hashlib.sha256).hexdigest()
    line = signed + "|" + mac

    if len(line) > TG_LINE_MAX:
        raise SystemExit(
            f"error: the line is {len(line)} bytes and the device refuses "
            f"anything over {TG_LINE_MAX}"
        )

    # The line only, on stdout, so it can be piped. Anything a human
    # needs to read goes to stderr.
    print(line)
    if args.add:
        print(f"note: counter {args.counter}; the device must currently hold less "
              f"than that", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
