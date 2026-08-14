#!/usr/bin/env python3
"""Quality checks that cannot be byte-compared against a reference.

  check_quality.py selftest  --cmdtozjs BIN --jbverify BIN
      PrintSelfTestPage must emit a decodable, non-blank ZjStream page.

  check_quality.py diffuse   --filter BIN --mkraster BIN --jbverify BIN
      A gradient page must be auto-dithered (Floyd-Steinberg), i.e. the
      decoded page contains many black/white transitions.
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def load_mkfixtures():
    spec = importlib.util.spec_from_file_location(
        "mkfixtures", os.path.join(HERE, "mkfixtures.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def cmd_selftest(args):
    with tempfile.TemporaryDirectory(prefix="p1102test-") as td:
        out = os.path.join(td, "selftest.zjs")
        with open(out, "wb") as fout:
            subprocess.run(
                [args.cmdtozjs, "1", "user", "SelfTest", "1", ""],
                input=b"#CUPS-COMMAND\nPrintSelfTestPage\n",
                stdout=fout, check=True)
        mode = "--selftest" if args.selftest_blocks else "--any"
        r = subprocess.run([args.jbverify, mode, out])
        if r.returncode != 0:
            print("FAIL selftest", file=sys.stderr)
            return 1
    print("PASS selftest" + ("-blocks" if args.selftest_blocks else ""))
    return 0


def cmd_diffuse(args):
    with tempfile.TemporaryDirectory(prefix="p1102test-") as td:
        load_mkfixtures().generate(td)
        raster = os.path.join(td, "grad.raster")
        with open(os.path.join(td, "grad.pgm"), "rb") as fin, \
                open(raster, "wb") as fout:
            subprocess.run([args.mkraster, "-g"], stdin=fin, stdout=fout,
                           check=True)
        zjs = os.path.join(td, "grad.zjs")
        with open(raster, "rb") as fin, open(zjs, "wb") as fout:
            subprocess.run(
                [args.filter, "1", "user", "Gradient", "1", args.opts],
                stdin=fin, stdout=fout, check=True)
        mode = "--not-dithered" if args.expect == "not-dithered" else None
        cmd = [args.jbverify] + ([mode] if mode else []) + [zjs]
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print(f"FAIL {args.expect or 'diffuse-auto'}", file=sys.stderr)
            return 1
    print("PASS " + (args.expect or "diffuse-auto"))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)

    p1 = sub.add_parser("selftest")
    p1.add_argument("--cmdtozjs", required=True)
    p1.add_argument("--jbverify", required=True)
    p1.add_argument("--selftest-blocks", action="store_true")

    p2 = sub.add_parser("diffuse")
    p2.add_argument("--filter", required=True)
    p2.add_argument("--mkraster", required=True)
    p2.add_argument("--jbverify", required=True)
    p2.add_argument("--opts", default="")
    p2.add_argument("--expect", default="dithered",
                    choices=["dithered", "not-dithered"])

    args = ap.parse_args()
    if args.mode == "selftest":
        return cmd_selftest(args)
    return cmd_diffuse(args)


if __name__ == "__main__":
    sys.exit(main())
