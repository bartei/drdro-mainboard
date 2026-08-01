#
# PlatformIO extra script: inject the bootloader version from git at build time.
#
# Defines BL_VERSION="<git describe>" so the bootloader's `version` command reports
# a real version instead of the baseline's hardcoded "bootloader" fallback — a host
# deciding whether a bootloader supports a feature has something to ask.
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

env.Append(CPPDEFINES=[("BL_VERSION", env.StringifyMacro(version))])
print("BL_VERSION = %s" % version)
