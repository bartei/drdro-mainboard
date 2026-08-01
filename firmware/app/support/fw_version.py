#
# PlatformIO extra script: inject the firmware version from git at build time.
#
# Defines FW_VERSION="<git describe>" so the `version` command can report it.
# Falls back to "unknown" outside a git checkout.
#
Import("env")
import os
import subprocess

# CI can pin the version via the FW_VERSION env var (e.g. the release workflow);
# otherwise derive it from git.
version = os.environ.get("FW_VERSION")
if not version:
    try:
        version = (
            subprocess.check_output(
                ["git", "describe", "--tags", "--always", "--dirty"],
                cwd=env.subst("$PROJECT_DIR"),
                stderr=subprocess.DEVNULL,
            )
            .strip()
            .decode()
        )
    except Exception:
        version = "unknown"

env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
print("FW_VERSION = %s" % version)
