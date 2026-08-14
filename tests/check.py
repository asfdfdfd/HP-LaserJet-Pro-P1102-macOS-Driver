#!/usr/bin/env python3
"""Byte-identity check for the rastertozjs filter.

Generates fixtures, runs filter(raster) and refmain(pbm), and requires the
two ZjStream documents to be byte-identical (PJL timestamp normalized).

Usage:
  check.py NAME --filter BIN --refmain BIN --mkraster BIN
          [--pbm FIXTURE] [--raster-pbm FIXTURE] [--ref-pbm FIXTURE]
          [--raster-args ".."] [--refargs ".."] [--opts ".."]
"""
import argparse
import importlib.util
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

REFARGS_DEFAULT = "-z2 -P -L0 -r600x600 -g5100x6600 -p1 -m1 -s7 -n1"


def load_mkfixtures():
    spec = importlib.util.spec_from_file_location(
        "mkfixtures", os.path.join(HERE, "mkfixtures.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def normalize(path):
    data = open(path, "rb").read()
    return re.sub(rb"JobAttr4=\d{14}", b"JobAttr4=N", data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--filter", required=True)
    ap.add_argument("--refmain", required=True)
    ap.add_argument("--mkraster", required=True)
    ap.add_argument("--pbm", default="test600.pbm")
    ap.add_argument("--raster-pbm", default=None)
    ap.add_argument("--ref-pbm", default=None)
    ap.add_argument("--raster-args", default="")
    ap.add_argument("--refargs", default=REFARGS_DEFAULT)
    ap.add_argument("--opts", default="")
    ap.add_argument("--copies", type=int, default=1)
    args = ap.parse_args()

    raster_pbm = args.raster_pbm or args.pbm
    ref_pbm = args.ref_pbm or args.pbm

    with tempfile.TemporaryDirectory(prefix="p1102test-") as td:
        load_mkfixtures().generate(td)

        raster = os.path.join(td, args.name + ".raster")
        raster_in = open(os.path.join(td, raster_pbm), "rb")
        with open(raster, "wb") as out:
            cmd = [args.mkraster] + (args.raster_args.split()
                                     if args.raster_args else [])
            subprocess.run(cmd, stdin=raster_in, stdout=out, check=True)
        raster_in.close()

        out_zjs = os.path.join(td, "out.zjs")
        with open(raster, "rb") as fin, open(out_zjs, "wb") as fout:
            subprocess.run(
                [args.filter, "1", "test", args.name, str(args.copies),
                 args.opts],
                stdin=fin, stdout=fout, check=True)

        ref_zjs = os.path.join(td, "ref.zjs")
        with open(os.path.join(td, ref_pbm), "rb") as fin, \
                open(ref_zjs, "wb") as fout:
            subprocess.run([args.refmain] + args.refargs.split(),
                           stdin=fin, stdout=fout, check=True)

        if normalize(out_zjs) != normalize(ref_zjs):
            print(f"FAIL {args.name}: outputs differ", file=sys.stderr)
            return 1
    print(f"PASS {args.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
