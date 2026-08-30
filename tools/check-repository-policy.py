"""Run the repository-policy checks locally, before pushing.

.github/workflows/repository-policy.yml enforces these on every push. Finding out
from a failed CI run costs a round trip and leaves a red mark on the history, so
run this first. It mirrors the workflow's checks, not a reinterpretation of them.

The rule that is easiest to trip: neither project source NOR CMake may carry
comments. Rationale belongs in focused files under docs/. A single explanatory
line added to a .cmake file fails the build.
"""
import io
import os
import re
import subprocess
import sys

CODE_EXTENSIONS = (".c", ".cpp", ".h", ".hpp", ".in", ".inl")
FOMOD_TEXT = (".xml", ".ini", ".txt")
EM_DASH = "\u2014"


def read(path):
    return io.open(path, encoding="utf-8", errors="replace").read()


def main():
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True).stdout.strip()
    if not root:
        print("not a git repository")
        return 2
    os.chdir(root)
    tracked = [t for t in subprocess.run(["git", "ls-files"], capture_output=True,
                                         text=True).stdout.split("\n") if t.strip()]
    failures = []

    hits = [t for t in tracked if t.lower().endswith(CODE_EXTENSIONS)
            and re.search(r"//|/\*|\*/", read(t))]
    if hits:
        failures.append(("source comments", hits))

    cmake = [t for t in tracked
             if t == "CMakeLists.txt" or t.endswith("/CMakeLists.txt")
             or t.endswith(".cmake")]
    hits = [t for t in cmake if re.search(r"^\s*#", read(t), re.M)]
    if hits:
        failures.append(("CMake comments", hits))

    fomod = [t for t in tracked if t.startswith("fomod/")
             and os.path.splitext(t)[1].lower() in FOMOD_TEXT]
    hits = [t for t in fomod if EM_DASH in read(t)]
    if hits:
        failures.append(("em dashes in installer text", hits))

    if os.path.exists("Licenses/PROJECT-LICENSE.txt"):
        failures.append(("publication-blocker licence present",
                         ["Licenses/PROJECT-LICENSE.txt"]))

    broken = []
    for doc in [t for t in tracked if t.lower().endswith(".md")]:
        for match in re.finditer(r"\]\(([^)#:]+?)(?:#[^)]*)?\)", read(doc)):
            target = match.group(1).strip()
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            if not os.path.exists(
                    os.path.normpath(os.path.join(os.path.dirname(doc), target))):
                broken.append("%s -> %s" % (doc, target))
    if broken:
        failures.append(("broken local Markdown links", broken))

    whitespace = subprocess.run(["git", "diff", "--check"],
                                capture_output=True, text=True).stdout.strip()
    if whitespace:
        failures.append(("whitespace errors", whitespace.split("\n")[:10]))

    if not failures:
        print("repository policy: PASS (%d tracked files checked)" % len(tracked))
        return 0
    for name, items in failures:
        print("FAIL %s:" % name)
        for item in items[:12]:
            print("       " + item)
    return 1


if __name__ == "__main__":
    sys.exit(main())
