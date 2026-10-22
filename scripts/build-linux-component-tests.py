"""Build every configured component and fast policy target before the full Linux package build."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def component_targets(build):
    reply = Path(build) / ".cmake/api/v1/reply"
    indices = list(reply.glob("index-*.json"))
    if not indices:
        raise ValueError("CMake component codemodel reply is missing")
    index = json.loads(max(indices, key=lambda p: p.stat().st_mtime_ns).read_text())
    model_file = index["reply"]["codemodel-v2"]["jsonFile"]
    model = json.loads((reply / model_file).read_text())
    configurations = model["configurations"]
    if len(configurations) != 1:
        raise ValueError("Linux package requires one configured build type")
    targets = sorted({t["name"] for t in configurations[0]["targets"] if re.fullmatch(r"test_(?:component|fast)_[A-Za-z0-9_]+", t["name"])})
    if "test_component_linux_routed_link" not in targets:
        raise ValueError("Required Linux routed-link component was not configured")
    return targets


def host_artifacts(build):
    reply = Path(build) / ".cmake/api/v1/reply"
    index = json.loads(max(reply.glob("index-*.json"), key=lambda p: p.stat().st_mtime_ns).read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    host = next(t for t in model["configurations"][0]["targets"] if t["name"] == "sunshine")
    return {a["path"] for a in json.loads((reply / host["jsonFile"]).read_text())["artifacts"]}


def test_targets(build):
    """Every input of the default build except the host executable: what ctest needs, without the package."""
    query = subprocess.run(["ninja", "-C", str(build), "-t", "query", "all"], check=True, capture_output=True, text=True).stdout
    inputs = query.split("input:", 1)[1].split("outputs:", 1)[0].splitlines()[1:]
    targets = [line.strip().lstrip("|").strip() for line in inputs if line.strip()]
    host = host_artifacts(build)
    if not host & set(targets):
        raise ValueError(f"The host executable {sorted(host)} is not in the default build")
    return [t for t in targets if t not in host]


def main(build, mode="components"):
    targets = component_targets(build) if mode == "components" else test_targets(build)
    print(f"Early test compile gate: {len(targets)} targets" if mode == "components" else f"Test build without the host executable: {len(targets)} targets", flush=True)
    # CI sets _keep_going=ON so one run reports every compile error, not just the first.
    native = ["--", "-k", "0"] if os.environ.get("_keep_going") == "ON" else []
    subprocess.run(["cmake", "--build", str(build), "--target", *targets, *native], check=True)


if __name__ == "__main__":
    # build-linux-component-tests.py <build dir>, or build-linux-component-tests.py tests <build dir>
    main(sys.argv[2], "tests") if sys.argv[1] == "tests" else main(sys.argv[1])
