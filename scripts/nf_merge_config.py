#!/usr/bin/env python3

# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.

"""
Merge a board defconfig with optional config overlays into a full .config.

Usage:
    python nf_merge_config.py <kconfig_root> <defconfig> <output_config> [overlay ...]

Arguments:
    kconfig_root   Path to the root Kconfig file (e.g. /path/to/nf-interpreter/Kconfig)
    defconfig      Path to the board/target minimal defconfig
    output_config  Path where the resulting full .config should be written
    overlay        Zero or more config fragments applied on top of the defconfig
                   in the given order (later overlays win); non-existent paths
                   and empty arguments are silently skipped

Overlays are loaded with Kconfig's non-replacing load (replace=False), so only
the symbols explicitly listed in an overlay are modified.
"""
import sys
import os

# Allow running from any directory — kconfiglib needs srctree when Kconfig
# files use relative `source` directives.  The caller (NF_Kconfig.cmake) sets
# this via WORKING_DIRECTORY, but guard against direct invocations.
if "srctree" not in os.environ:
    os.environ["srctree"] = os.getcwd()

import kconfiglib


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    kconfig_root, defconfig_path, output_config = sys.argv[1:4]
    overlays = sys.argv[4:]

    if not os.path.isfile(kconfig_root):
        print(f"Error: Kconfig root not found: {kconfig_root}", file=sys.stderr)
        sys.exit(1)

    if not os.path.isfile(defconfig_path):
        print(f"Error: defconfig not found: {defconfig_path}", file=sys.stderr)
        sys.exit(1)

    kconf = kconfiglib.Kconfig(kconfig_root, warn=False)
    kconf.load_config(defconfig_path)

    for overlay in overlays:
        if overlay and os.path.isfile(overlay):
            kconf.load_config(overlay, replace=False)

    kconf.write_config(output_config)


if __name__ == "__main__":
    main()
