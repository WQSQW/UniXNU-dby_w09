#!/usr/bin/env python3
"""Build the QcomXnuUsbDebug CDC-ACM console archive from the live XNU compile record.

官方配方就是 UniXNU 组件目录里的那条 spec（community-components/UniXNU/components.toml:513-534）：
调用模块自己的 scripts/build.py，输入 pe_kprintf.o.json 的真实编译记录，输出
{tree}/.cache/qcom-usb-debug/libQcomXnuUsbDebug.a。本板用的那份 core checkout 的
components.toml 里还没有这条 spec，而 components.toml 由 scripts/unixnu/tree.py:assemble 只从
core 软链进来，设备层不能添加组件规格；所以这里按分层规则把同一条命令放进设备仓的
[kernel] archives 脚本位（scripts/unixnu/kernel.py:archive_plan 对非 component: 条目走的路径），
命令、参数与 spec 逐字一致。

模块自己规定：--output 必须在 tmp/ 或 .cache/ 下、必须在源码目录之外，且 --replace 是给
「每次增量都复用同一路径」的目录驱动的构建用的（drivers/QcomXnuUsbDebug/scripts/build.py:--replace）。
--ar/--nm 不写死：build.py 先从 PATH 取 llvm-ar/llvm-nm，取不到再退回编译记录里 clang 的同目录，
两个候选在本机都存在，把机器相关的绝对路径写进发布的脚本反而是自制。
"""
import subprocess
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
MODULE = TREE / "kits" / "dby_w09" / "QcomXnuUsbDebug"
RECORD = (TREE / "build" / "xnu" / "ios-obj" / "DEVELOPMENT_ARM64_BCM2837"
          / "pexpert" / "DEVELOPMENT" / "pe_kprintf.o.json")
OUTPUT = TREE / ".cache" / "qcom-usb-debug"


def main():
    build_py = MODULE / "scripts" / "build.py"
    if not build_py.is_file():
        raise SystemExit(f"QcomXnuUsbDebug module missing: {build_py}")
    if not RECORD.is_file():
        raise SystemExit(
            f"compile record missing: {RECORD}\n"
            "the archive replays the selected XNU's real flags, so it needs the first "
            "(bootstrap) pass of the kernel build to have left this record behind")
    command = [sys.executable, str(build_py),
               "--compile-record", str(RECORD),
               "--soc", "soc/Sm8250.h",
               "--output", str(OUTPUT),
               "--replace"]
    subprocess.run(command, check=True)
    archive = OUTPUT / "libQcomXnuUsbDebug.a"
    if not archive.is_file():
        raise SystemExit(f"build.py left no archive at {archive}")
    print(archive)


if __name__ == "__main__":
    main()
