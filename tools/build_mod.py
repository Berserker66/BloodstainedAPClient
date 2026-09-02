"""Build and install the Bloodstained AP client."""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
PROJECT = REPOSITORY / "BloodstainedModdingSDK.vcxproj"
ARTIFACT = REPOSITORY / "x64/Release/BloodstainedAP.asi"
MSBUILD = Path(
    r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
    r"\MSBuild\Current\Bin\MSBuild.exe"
)
GAME_PROCESS = "BloodstainedRotN-Win64-Shipping.exe"
GAME_DIRECTORY = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\Bloodstained Ritual of the Night"
    r"\BloodstainedRotN\Binaries\Win64"
)
INSTALLED_PLUGIN = GAME_DIRECTORY / "plugins/BloodstainedAP.asi"


def positive_integer(value: str) -> int:
    jobs = int(value)
    if jobs < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return jobs


def sanitized_environment(jobs: int) -> dict[str, str]:
    environment: dict[str, str] = {}
    seen: set[str] = set()
    for key, value in os.environ.items():
        folded = key.casefold()
        if folded in seen or folded in {
            "codex_sandbox",
            "codex_sandbox_network_disabled",
        }:
            continue
        seen.add(folded)
        environment[key] = value
    environment["USERNAME"] = "fabia"
    environment["CL_MP"] = "true"
    environment["CL_MPCount"] = str(jobs)
    return environment


def game_is_running() -> bool:
    result = subprocess.run(
        ["tasklist", "/FI", f"IMAGENAME eq {GAME_PROCESS}", "/FO", "CSV", "/NH"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"tasklist failed: {result.stderr.strip()}")
    for row in csv.reader(result.stdout.splitlines()):
        if len(row) >= 2 and row[1].replace(",", "").isdigit():
            return True
    return False


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def install() -> str:
    if not ARTIFACT.is_file():
        raise FileNotFoundError(f"build succeeded without producing {ARTIFACT}")
    INSTALLED_PLUGIN.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ARTIFACT, INSTALLED_PLUGIN)
    artifact_hash = sha256(ARTIFACT)
    installed_hash = sha256(INSTALLED_PLUGIN)
    if artifact_hash != installed_hash:
        raise RuntimeError(
            f"installed plugin hash mismatch: {installed_hash} != {artifact_hash}"
        )
    return artifact_hash


def run_msbuild(target: str, jobs: int) -> subprocess.CompletedProcess[str]:
    command = [
        str(MSBUILD),
        str(PROJECT),
        f"/t:{target}",
        "/p:Configuration=Release",
        "/p:Platform=x64",
        "/p:PlatformToolset=v145",
        f"/m:{jobs}",
    ]
    print(f"Running {target} with {jobs} compiler workers", flush=True)
    return subprocess.run(
        command,
        cwd=REPOSITORY,
        env=sanitized_environment(jobs),
        capture_output=True,
        text=True,
        check=False,
    )


def log_build_result(target: str, result: subprocess.CompletedProcess[str]) -> None:
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(result.stderr, end="" if result.stderr.endswith("\n") else "\n", file=sys.stderr)
    if result.returncode != 0:
        print(f"{target} failed with exit code {result.returncode}.", file=sys.stderr, flush=True)


def build(clean: bool, jobs: int) -> int:
    target = "Rebuild" if clean else "Build"
    result = run_msbuild(target, jobs)
    log_build_result(target, result)
    if result.returncode == 0 or clean:
        return result.returncode

    print("Incremental build failed; retrying with a clean rebuild.", file=sys.stderr, flush=True)
    result = run_msbuild("Rebuild", jobs)
    log_build_result("Rebuild", result)
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--clean",
        action="store_true",
        help="perform a full rebuild instead of the default incremental build",
    )
    parser.add_argument(
        "--jobs",
        type=positive_integer,
        default=16,
        metavar="N",
        help="maximum MSBuild and compiler workers (default: 16)",
    )
    args = parser.parse_args()

    if game_is_running():
        parser.error(f"{GAME_PROCESS} is running; exit the game before building")
    if not MSBUILD.is_file():
        parser.error(f"MSBuild was not found at {MSBUILD}")
    if not GAME_DIRECTORY.is_dir():
        parser.error(f"game directory was not found at {GAME_DIRECTORY}")

    build_result = build(args.clean, args.jobs)
    if build_result != 0:
        return build_result

    installed_hash = install()
    print(f"Installed: {INSTALLED_PLUGIN}")
    print(f"SHA-256: {installed_hash}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
