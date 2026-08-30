"""Compile every inline HLSL shader in the tree with the runtime's exact flags.

D3DCompile is called at runtime with cs_5_1, ENABLE_STRICTNESS and
WARNINGS_ARE_ERRORS. A shader that fails there does not fail the C++ build, it
fails silently in game, so nothing in ctest catches it. Worse, a failed pass may
be retried every frame. Run this before deploying a candidate.

Two traps this exists to catch, both hit on 2026-08-29:
  cs_5_1 rejects an early return that cs_5_0 accepts (X4000, potentially
  uninitialized variable), so testing against cs_5_0 gives a false pass.
  WARNINGS_ARE_ERRORS means any warning at all is fatal.
"""
import glob
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATTERN = re.compile(
    r'constexpr\s+std::string_view\s+(\w+)\s*=\s*R"\((.*?)\)";', re.S)


def find_fxc():
    roots = [
        r"C:\Program Files (x86)\Windows Kits\10\bin",
        r"C:\Program Files\Windows Kits\10\bin",
    ]
    found = []
    for base in roots:
        found += glob.glob(os.path.join(base, "*", "x64", "fxc.exe"))
    return sorted(found)[-1] if found else None


def profile_for(body, source):
    """Pick the profile the RUNTIME will use, not a guess.

    The compute profile is read from the D3DCompile call in the same file,
    because cs_5_0 and cs_5_1 disagree about early returns and assuming the
    stricter one reports failures that will never happen in game. Files mixing
    two compute profiles would defeat this; none currently do.
    """
    if "numthreads" in body:
        # A shader handed to ComputePass::create is compiled by ComputePass,
        # which uses cs_5_1, even though that literal lives in another file.
        # Getting this wrong is not a harmless mislabel: cs_5_0 accepts early
        # returns that cs_5_1 rejects, so assuming the local literal gives a
        # FALSE PASS and ships a shader that cannot compile in game.
        if '"cs_5_1"' in source:
            profile = "cs_5_1"
        elif "ComputePass" in source or "ComputeBindingLayout" in source:
            profile = "cs_5_1"
        else:
            profile = "cs_5_0"
        return profile, "main"
    if "SV_Target" in body or "SV_Depth" in body:
        return "ps_5_0", "main"
    if "SV_VertexID" in body:
        return "vs_5_0", "main"
    return None, None


def main():
    fxc = find_fxc()
    if fxc is None:
        print("fxc.exe not found; cannot verify shaders")
        return 2

    failures = 0
    checked = 0
    for path in glob.glob(os.path.join(ROOT, "src", "**", "*.cpp"),
                          recursive=True):
        source = io.open(path, encoding="utf-8", errors="replace").read()
        for name, body in PATTERN.findall(source):
            profile, entry = profile_for(body, source)
            if profile is None:
                continue
            checked += 1
            temp = os.path.join(
                os.environ.get("TEMP", "."), "ufgu-shader-%s.hlsl" % name)
            io.open(temp, "w", encoding="utf-8", newline="\n").write(body)
            result = subprocess.run(
                [fxc, "/T", profile, "/E", entry, "/Ges", "/O3", "/WX",
                 "/nologo", "/Fo", "NUL", temp],
                capture_output=True, text=True)
            label = "%s (%s)" % (name, os.path.basename(path))
            if result.returncode == 0:
                print("  ok   %s [%s]" % (label, profile))
            else:
                failures += 1
                print("  FAIL %s [%s]" % (label, profile))
                for line in (result.stdout + result.stderr).splitlines():
                    if line.strip():
                        print("       " + line.replace(temp, name))
                lines = body.split("\n")
                hit = re.search(r"\((\d+),\d+\)", result.stdout + result.stderr)
                if hit:
                    n = int(hit.group(1))
                    for i in range(max(1, n - 2), min(len(lines), n + 2) + 1):
                        mark = "    >>" if i == n else "      "
                        print("%s %3d | %s" % (mark, i, lines[i - 1]))

    print("\n%d inline shaders checked, %d failed" % (checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
