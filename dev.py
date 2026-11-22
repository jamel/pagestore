#!/usr/bin/env python3
"""Build and test page-store using its CMake presets."""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent
RED = 31
GREEN = 32
YELLOW = 33
BLUE = 34
CYAN = 36
COLOR_ALLOWED = not os.environ.get("NO_COLOR") and os.environ.get("TERM") != "dumb"
COLOR_ENABLED = COLOR_ALLOWED and sys.stderr.isatty()


def colorize(text, color):
    return f"\x1b[{color}m{text}\x1b[0m"


def color_help(text):
    # Add color after argparse wraps the lines so escape codes don't count toward the width.
    text = re.sub(
        r"^usage:|^\S[^\n]*:$", lambda match: colorize(match[0], CYAN), text,
        flags=re.MULTILINE,
    )
    text = re.sub(
        r"^( {4})([a-z][a-z0-9-]*)(?= {2,}|$)",
        lambda match: match[1] + colorize(match[2], GREEN),
        text,
        flags=re.MULTILINE,
    )
    return re.sub(
        r"(?<![\w-])(?:--[\w-]+|-\w)(?![\w-])", lambda match: colorize(match[0], YELLOW), text,
    )


class HelpParser(argparse.ArgumentParser):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        if hasattr(self, "color"):
            # Keep argparse from adding its own colors on newer Python versions.
            self.color = False

    def print_help(self, file=None):
        if file is None:
            file = sys.stdout
        if COLOR_ALLOWED and file.isatty():
            file.write(color_help(self.format_help()))
        else:
            super().print_help(file)


def positive_int(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def make_parser():
    parser = HelpParser(
        description="Build and test page-store. The default preset is debug."
    )
    commands = parser.add_subparsers(dest="command", metavar="COMMAND")
    configure = commands.add_parser("configure", help="configure a build directory")
    build = commands.add_parser("build", help="configure and build")
    test = commands.add_parser("test", help="configure, build, and run tests")
    clean = commands.add_parser("clean", help="remove generated build outputs")
    commands.add_parser("presets", help="list available CMake and CTest presets")

    for command in (configure, build, test, clean):
        selection = command.add_mutually_exclusive_group()
        preset_help = "use the named preset (default: debug)"
        if command is not configure:
            preset_help = "use the named preset or 'all' (default: debug)"
        selection.add_argument(
            "-p", "--preset", default="debug", metavar="NAME",
            help=preset_help,
        )
        selection.add_argument(
            "--release", dest="preset", action="store_const", const="release",
            help="use the release preset",
        )

    for command in (build, test):
        command.add_argument(
            "-j", "--jobs", type=positive_int, metavar="N",
            help="maximum parallel jobs for build and test steps",
        )

    build.add_argument("--target", metavar="NAME", help="build a specific target")
    test.add_argument("filter", nargs="?", metavar="FILTER", help="test name regex")
    test.add_argument("--label", metavar="REGEX", help="CTest label regex")
    test.add_argument(
        "--no-build", action="store_true", help="use the existing configuration and build",
    )
    test.add_argument(
        "--list", action="store_true", help="list test names without building or running them",
    )
    test.add_argument("-v", "--verbose", action="store_true", help="show all test output")
    return parser


def report(message, *, color=YELLOW):
    if COLOR_ENABLED:
        message = colorize(message, color)
    print(message, file=sys.stderr, flush=True)


def run(command, *, stdout=None, capture_stderr=False):
    report(f"+ {shlex.join(command)}", color=BLUE)
    try:
        return subprocess.run(
            command, cwd=PROJECT_ROOT, check=True, text=True,
            stdout=stdout,
            stderr=subprocess.PIPE if capture_stderr else None,
        )
    except FileNotFoundError:
        report(f"Could not find {command[0]} on PATH.", color=RED)
    except OSError as error:
        report(f"Could not run {command[0]} ({error.strerror}).", color=RED)
    sys.exit(1)


def clean_preset(preset):
    command = ["cmake", "--build", "--preset", preset, "--target", "clean"]
    try:
        result = run(command, capture_stderr=True)
    except subprocess.CalledProcessError as error:
        missing_directory = re.fullmatch(r"Error: (.+) is not a directory\n?", error.stderr)
        if (
            error.returncode == 1
            and missing_directory
            and not Path(missing_directory[1]).exists()
        ):
            report(f"No build directory for preset {preset}.")
            return
        sys.stderr.write(error.stderr)
        raise
    sys.stderr.write(result.stderr)


def run_tests(args, preset, parallel):
    test = ["ctest", "--preset", preset]
    if args.list:
        test.append("--show-only=json-v1")
    else:
        test.extend(["--output-on-failure", "--no-tests=error", *parallel])
    if args.filter is not None:
        test.extend(["-R", args.filter])
    if args.label is not None:
        test.extend(["-L", args.label])

    if args.list:
        # The JSON listing includes test names before their executables are built.
        result = run(test, stdout=subprocess.PIPE)
        tests = json.loads(result.stdout)["tests"]
        for case in tests:
            print(case["name"])
        print(f"Total tests: {len(tests)}", flush=True)
    else:
        if args.verbose:
            test.append("--verbose")
        run(test)


def run_preset(args, preset):
    if args.command == "clean":
        clean_preset(preset)
        return

    configure = ["cmake", "--preset", preset]
    if args.command == "configure":
        run(configure)
        return

    parallel = ["--parallel", str(args.jobs)] if args.jobs is not None else []
    listing = args.command == "test" and args.list
    if args.command == "build" or not args.no_build:
        run(configure, stdout=sys.stderr if listing else None)
        if not listing:
            build = ["cmake", "--build", "--preset", preset, *parallel]
            if args.command == "build" and args.target is not None:
                build.extend(["--target", args.target])
            run(build)

    if args.command == "test":
        run_tests(args, preset, parallel)


def run_all_presets(args):
    preset_type = "build" if args.command == "clean" else args.command
    listing = run(["cmake", f"--list-presets={preset_type}"], stdout=subprocess.PIPE)
    # A line can end in a display name, as in '  "debug" - Debug build'.
    presets = re.findall(r'^  "(.+?)"(?: +- .*)?$', listing.stdout, re.MULTILINE)
    if not presets:
        report(f"No available {preset_type} presets.", color=RED)
        sys.exit(1)

    failures = {}
    for preset in presets:
        report(f"\n== {args.command} preset {preset} ==")
        try:
            run_preset(args, preset)
        except subprocess.CalledProcessError as error:
            failures[preset] = error

    if failures:
        report(f"\nFailed presets ({len(failures)} of {len(presets)})", color=RED)
        for preset, error in failures.items():
            report(f"  {preset} (exit {error.returncode})", color=RED)
        raise next(iter(failures.values()))


def main():
    parser = make_parser()
    args = parser.parse_args()
    if args.command is None:
        parser.print_help()
    elif args.command == "presets":
        run(["cmake", "--list-presets=all"])
    elif args.command in ("build", "test", "clean") and args.preset == "all":
        run_all_presets(args)
    else:
        run_preset(args, args.preset)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        status = error.returncode
        sys.exit(status if status >= 0 else 128 - status)
    except KeyboardInterrupt:
        sys.exit(130)
