#!/usr/bin/env python3
"""p1102ctl - manage the open-source HP LaserJet P1102 driver.

Build (Meson), install, printer queue, status, EWS proxy and test page.

Requires Python 3.9+ (stdlib only).  Privileged subcommands re-exec
themselves via sudo.

  p1102ctl setup            build, install files and register the queue
  p1102ctl install          build + install driver files only
  p1102ctl uninstall        remove driver files
  p1102ctl add-printer      register the CUPS queue
  p1102ctl remove-printer   remove the CUPS queue
  p1102ctl status           toner/status (EWS over USB)
  p1102ctl ews [path]       local HTTP proxy to the printer's EWS
  p1102ctl print-test-page  print the self-test page
  p1102ctl package          build the release .pkg
  p1102ctl version
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

VERSION = "1.1.0"

BUNDLE = "/Library/Printers/HP/p1102raster.bundle"
BIN_DIR = BUNDLE + "/Contents/MacOS"
PPD_DIR = "/Library/Printers/PPDs/Contents/Resources"
PPD_NAME = "HP LaserJet Professional P1102.ppd"
PPD_PATH = os.path.join(PPD_DIR, PPD_NAME)
QUEUE = "HP_P1102"
QUEUE_DESC = "HP LaserJet Professional P1102 (open source driver)"

BINARIES = ("rastertozjs", "p1102status", "commandtozjs", "ewsproxy",
            "p1102cmd")


def repo_root():
    """Repository root if running from a checkout, else None."""
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (here, os.path.dirname(here)):
        if os.path.exists(os.path.join(d, "meson.build")):
            return d
    return None


def need_root():
    """Re-exec via sudo when not running as root.  Never returns."""
    if os.geteuid() != 0:
        args = [sys.executable, os.path.abspath(__file__)] + sys.argv[1:]
        os.execvp("sudo", ["sudo"] + args)


def run(cmd, **kw):
    print("==> " + " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def meson_build(root, build_dir):
    if not os.path.exists(os.path.join(build_dir, "build.ninja")):
        run(["meson", "setup", build_dir], cwd=root)
    run(["meson", "compile", "-C", build_dir], cwd=root)


def find_usb_uri():
    out = subprocess.run(["lpinfo", "-v"], capture_output=True, text=True,
                         check=False).stdout
    for line in out.splitlines():
        if "usb://" in line.lower() and "p1102" in line.lower():
            return line.split()[-1]
    return None


def installed_or_built(name, root, build_dir):
    path = os.path.join(BIN_DIR, name)
    if os.path.exists(path):
        return path
    if root:
        p = os.path.join(root, build_dir, name)
        if os.path.exists(p):
            return p
    return None


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def meson_cmd():
    """Absolute path to meson, for sudo invocations (sudo may not see
    /opt/homebrew/bin)."""
    return shutil.which("meson") or "meson"


def cmd_setup(args):
    root = repo_root()
    if not root:
        sys.exit("p1102ctl: not a repository checkout; use a release .pkg")
    build_dir = os.path.join(root, "build")
    meson_build(root, build_dir)
    subprocess.run(["sudo", sys.executable, os.path.abspath(__file__),
                    "install"], check=True)
    subprocess.run(["sudo", sys.executable, os.path.abspath(__file__),
                    "add-printer"], check=True)


def cmd_install(args):
    root = repo_root()
    if not root:
        sys.exit("p1102ctl: not a repository checkout; use a release .pkg")
    build_dir = os.path.join(root, "build")
    if os.geteuid() == 0:
        run(["meson", "install", "-C", build_dir], cwd=root)
        print(f"==> installed to {BUNDLE}")
        return
    meson_build(root, build_dir)
    subprocess.run(["sudo", meson_cmd(), "install", "-C", build_dir],
                   check=True)
    print(f"==> installed to {BUNDLE}")


def cmd_uninstall(args):
    need_root()
    for path in (BUNDLE, PPD_PATH):
        if os.path.isdir(path):
            shutil.rmtree(path)
        elif os.path.exists(path):
            os.unlink(path)
    print("==> removed driver files")


def cmd_add_printer(args):
    need_root()
    uri = find_usb_uri()
    if not uri:
        print("WARNING: P1102 USB device not found via lpinfo.")
        print("         Add the printer in System Settings -> Printers &")
        print("         Scanners (driver: HP LaserJet Pro P1102,")
        print("         rastertozjs (open source)).")
        return 1
    run(["lpadmin", "-p", QUEUE, "-E", "-v", uri, "-P", PPD_PATH,
         "-o", "printer-is-shared=false", "-D", QUEUE_DESC])
    return 0


def cmd_remove_printer(args):
    need_root()
    run(["lpadmin", "-x", QUEUE])


def cmd_status(args):
    tool = installed_or_built("p1102status", repo_root(), "build")
    if not tool:
        sys.exit("p1102ctl: p1102status not found (install the driver first)")
    os.execv(tool, [tool, "--json"] if args.json else [tool])


def cmd_ews(args):
    tool = installed_or_built("ewsproxy", repo_root(), "build")
    if not tool:
        sys.exit("p1102ctl: ewsproxy not found (install the driver first)")
    argv = [tool]
    if args.path:
        argv.append(args.path)
    os.execv(tool, argv)


def cmd_print_test_page(args):
    tool = installed_or_built("p1102cmd", repo_root(), "build")
    if not tool:
        sys.exit("p1102ctl: p1102cmd not found (install the driver first)")
    os.execv(tool, [tool, args.queue, "PrintSelfTestPage"])


def cmd_package(args):
    root = repo_root()
    if not root:
        sys.exit("p1102ctl: not a repository checkout")
    build_dir = os.path.join(root, "build")
    meson_build(root, build_dir)

    stage = tempfile.mkdtemp(prefix="p1102pkg-")
    try:
        run(["meson", "install", "-C", build_dir, "--destdir", stage],
            cwd=root)
        payload = os.path.join(stage, "Library")
        scripts = tempfile.mkdtemp(prefix="p1102scripts-")
        try:
            postinstall = os.path.join(scripts, "postinstall")
            with open(postinstall, "w") as f:
                f.write("#!/bin/sh\n"
                        'QUEUE="HP_P1102"\n'
                        'PPD="/Library/Printers/PPDs/Contents/Resources/'
                        'HP LaserJet Professional P1102.ppd"\n'
                        "URI=$(lpinfo -v 2>/dev/null | grep -i usb:// | "
                        "grep -i p1102 | head -1 | awk '{print $2}')\n"
                        "if [ -n \"$URI\" ]; then\n"
                        "    lpadmin -p \"$QUEUE\" -E -v \"$URI\" -P \"$PPD\" "
                        "-o printer-is-shared=false \\\n"
                        '        -D "HP LaserJet Professional P1102 '
                        '(open source driver)"\n'
                        "fi\n"
                        "exit 0\n")
            os.chmod(postinstall, 0o755)
            pkg = os.path.join(root, f"HP-LaserJet-Pro-P1102-{VERSION}.pkg")
            subprocess.run(["sudo", "/usr/sbin/chown", "-R", "root:wheel",
                            payload], check=True)
            subprocess.run(["sudo", "/usr/sbin/pkgbuild",
                            "--root", payload,
                            "--identifier", "org.opencode.hp-p1102-driver",
                            "--version", VERSION,
                            "--ownership", "recommended",
                            "--scripts", scripts,
                            pkg], check=True)
            print(f"==> {pkg}")
        finally:
            shutil.rmtree(scripts, ignore_errors=True)
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def cmd_version(args):
    print(VERSION)


def main():
    ap = argparse.ArgumentParser(
        prog="p1102ctl",
        description="Manage the open-source HP LaserJet P1102 driver")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("setup", help="build, install and register the queue")
    sub.add_parser("install", help="build and install driver files")
    sub.add_parser("uninstall", help="remove driver files")
    sub.add_parser("add-printer", help="register the CUPS queue")
    sub.add_parser("remove-printer", help="remove the CUPS queue")

    p = sub.add_parser("status", help="printer status and toner level")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("ews", help="open the printer EWS via a local proxy")
    p.add_argument("path", nargs="?", default=None)

    p = sub.add_parser("print-test-page", help="print the self-test page")
    p.add_argument("--queue", default=QUEUE)

    sub.add_parser("package", help="build the release .pkg")
    sub.add_parser("version", help="print version")

    args = ap.parse_args()
    dispatch = {
        "setup": cmd_setup,
        "install": cmd_install,
        "uninstall": cmd_uninstall,
        "add-printer": cmd_add_printer,
        "remove-printer": cmd_remove_printer,
        "status": cmd_status,
        "ews": cmd_ews,
        "print-test-page": cmd_print_test_page,
        "package": cmd_package,
        "version": cmd_version,
    }
    sys.exit(dispatch[args.cmd](args))


if __name__ == "__main__":
    main()
