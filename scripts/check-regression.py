#!/usr/bin/env python3
# Pre-flash regression gate: proves that the artifact about to be written still carries every
# capability this tablet has already been observed working, and that no boot arg or payload input
# silently disappeared on the way from board.toml to payload.json.
#
# It answers one question: 这次刷的东西，有没有把以前弄好的东西弄丢。
# Every literal below was counted inside the shipped kernel.macho of the accepted boot (ord203,
# 2026-11-08) -- guessed prefixes are not in the list, because they return 0 and would read as green.
#
# Usage: python3.11 scripts/check-regression.py [-D <device-repo>] [--verbose]
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

CRITICAL_ARGS = {
    "dby-battery=2": "电池/充电图标（DbyBattery 双 tier + ICL 投票）",
    "dby-touch=8": "触摸阶段梯（DbyTouch）",
    "sheng-touch-hid=1": "触摸进 HID 消费端",
    "dby-touch-se=1": "触摸 I2C 从机选择",
    "sheng-buttons=1": "按键行",
    "dby-bl=1": "亮度控制",
    "offline-legacy-unit-scale=2": "界面大小 / display-scale",
    "boot-progress=2": "诊断页与锁屏画面",
    "sheng-gpu-addon=1": "GPU 附加包（显示后端）",
    "offline-mobile-graphics-load=1": "移动图形加载",
    "offline-mobile-graphics-client=1": "移动图形客户端",
    "dby-audio=2": "音频服务与共享环",
    "dby-pa=3": "smartPA 阶梯",
    "sheng-adsp=2": "ADSP 拉起（扬声器的前置）",
    "qcom-usb-debug=1": "COM4 内核串口判读",
    "sheng-data-reset=1": "Data 复位（删了会卡苹果标）",
    "native-ufs-root=1": "根卷匹配",
    "-apfs_shared_datavolume": "共享 Data 卷",
    "offline-apfs-load=1": "APFS 离线加载",
    "offline-hid-home=1": "Home 事件",
    "offline-hid-input=1": "HID 输入",
    "offline-key-events=1": "按键事件",
    "sheng-pmgr=2": "PMGR（AMFI 前置）",
    "armcpu-spi-ipi=1": "ArmCpu IPI（timer PPI 前置）",
    "sheng-smp=1": "多核",
    "panic_on_crash_disabled=1": "崩了还能读串口",
    "serial=3": "串口输出",
    "-enable_kprintf_spam": "kprintf 不被限流",
    "sheng-aks=1": "KeyStore",
    "sheng-log=1": "日志",
    "sheng-cpufreq=3": "EPSS cpufreq",
}

ARG_LIMIT_BYTES = 1000

# Row prefixes the tablet itself printed on 2026-11-08 (com4-ord203.txt, the boot that showed the
# lock screen, the charging badge, touch, brightness and interface size all working). If one of
# these is not in the bytes about to be flashed, that boot's driver code is not in this artifact --
# which is exactly the failure the host-side marker check alone cannot see.
DEVICE_ROWS = [
    "DbyBattery: arb=", "DbyBattery: tier=", "DbyBattery: published soc=",
    "DbyBattery: icl measure", "DbyBattery: icl apply step=", "DbyBattery: icl ceiling",
    "DbyBattery: icl holding want=", "DbyBattery: read opcode=", "DbyBattery: write opcode=",
    "DbyBattery: rw channel ppid=", "[apr] os=", "[apr] tr=", "[apr] pa=", "[apr] step=", "[apr] copp=",
]


def out_line(name, ok, detail):
    return ("PASS  " if ok else "FAIL  ") + name + ("  " + detail if detail else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-D", "--device", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--out", default="/home/wqsqw/upstream/core/out/dby-w09/ipados-17.7.11-21H461/dby_w09")
    ap.add_argument("--baseline", default=str(Path(__file__).resolve().parent / "regression-baseline.json"))
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    dev = Path(a.device)
    out = Path(a.out)
    results = []
    note = lambda ok, name, detail="": results.append((ok, name, detail))

    # ---- 1. artifact identity: the kernel in the payload is the kernel we just built ----
    baseline_path = Path(a.baseline)
    base_doc = json.loads(baseline_path.read_text()) if baseline_path.is_file() and not a.write_baseline else {}

    pj = out / "payload/payload.json"
    if not pj.is_file():
        print("FAIL  payload.json missing at", pj)
        return 1
    rec = json.loads(pj.read_text())
    km = out / "kernel/kernel.macho"
    if not km.is_file():
        print("FAIL  kernel.macho missing at", km)
        return 1
    ksha = hashlib.sha256(km.read_bytes()).hexdigest()
    note(ksha == rec["kernel_sha256"], "kernel.macho == payload.json kernel_sha256", ksha[:24])
    pi = out / "payload/payload.img"
    note(pi.is_file() and pi.stat().st_mtime > km.stat().st_mtime,
         "payload.img 晚于 kernel.macho（构建次序）",
         f"{km.stat().st_mtime_ns // 10**9} -> {pi.stat().st_mtime_ns // 10**9}" if pi.is_file() else "payload.img 不存在")

    # ---- 2. PMGR bridge second pass, else AMFI panics at ConfigurationSettings.cpp:235 ----
    sp = out / "tree/build/sheng-platform-power"
    second = (sp / "ShengPlatformPower.o").is_file()
    note(second, "内核已跑第二遍（ShengPlatformPower.o 在）", str(sp))

    # ---- 3. every accepted capability still compiled into the image ----
    raw = km.read_bytes()
    markers = base_doc.get("markers")
    if markers is None:
        markers = {
            "电池 presence 优先修复已发布状态": "DbyBattery: published soc=",
            "充电 ICL 测量/投票": "DbyBattery: icl measure:",
            "SPMI arbiter 普查（APID 通道）": "DbyBattery: arb=",
            "电池探针挂载": "[battery] probe start",
            "音频服务播放与音量计数": "[apr] os=",
            "ADSP/PA 阶梯": "[pa-adsp] ",
            "触摸驱动在镜像里": "DbyTouch",
            "触摸面板厂商路径": "Novatek",
            "亮度代码在镜像里": "backlight",
            "诊断页/锁屏渲染": "BootProgress",
            "界面大小换算": "unit-scale",
            "USB 调试控制台": "qcom-usb-debug",
            "音频服务类": "DbyAudio",
            "smartPA 表与阶梯在镜像里": "smartpa",
        }
    for name, lit in markers.items():
        n = raw.count(lit.encode())
        note(n > 0, f"标记串 {name}", f"'{lit}' x{n}")

    # ---- 3b. the same markers inside the object that actually gets written to preload ----
    # kernel.macho alone proves nothing about the flash: payload.img is the flashable file, and the
    # gate must read the bytes the tablet will boot. fastboot fetch is unsupported here, so this is
    # the strongest host-side identity proof available, and it closes the loop against the COM4 rows.
    pib = pi.read_bytes() if pi.is_file() else b""
    note(hashlib.sha256(pib).hexdigest() == rec.get("payload_sha256"),
         "payload.img == payload.json payload_sha256（这就是将刷的那份）",
         hashlib.sha256(pib).hexdigest()[:24] if pib else "payload.img 不存在")
    for name, lit in markers.items():
        n = pib.count(lit.encode())
        note(n > 0, f"将刷镜像含 {name}", f"'{lit}' x{n}")
    for lit in DEVICE_ROWS:
        note(pib.count(lit.encode()) > 0, "真机见过的行在此镜像里", lit)

    # ---- 4. args: board.toml intent == payload.json reality == accepted baseline ----
    bt = (dev / "board.toml").read_text(errors="replace")
    m = re.search(r"kernel_args = \[(.*?)\n\]", bt, re.S)
    authored = []
    if m:
        # Only whole-line entries count: the block carries line comments whose prose contains the
        # very argument names, and a quote-pairing regex across them invents bogus "arguments".
        authored = re.findall(r'^\s*"([^"]+)"\s*,?.*$', m.group(1), re.M)
    shipped = rec["kernel_args"].split()
    note(bool(authored), "board.toml 里读到 kernel_args 列表", f"{len(authored)} 项")
    missing_vs_toml = [x for x in authored if x not in shipped]
    extra_vs_toml = [x for x in shipped if x not in authored]
    note(not missing_vs_toml, "board.toml 的每个参数都进了 payload", ",".join(missing_vs_toml))
    note(not extra_vs_toml, "payload 没有 board.toml 之外的参数", ",".join(extra_vs_toml))
    blob = " ".join(shipped)
    note(len(blob) <= ARG_LIMIT_BYTES, f"命令行长度 <= {ARG_LIMIT_BYTES} 字节", f"{len(blob)} B")
    for arg, why in CRITICAL_ARGS.items():
        note(arg in shipped, f"关键参数 {arg}", why)
    if base_doc.get("args"):
        base = set(base_doc["args"])
        lost = sorted(base - set(shipped))
        note(not lost, "对照已验收基线没有丢参数", ",".join(lost))
        # Additions are advisory, never gate-red: an argument can be added on purpose, but one that
        # quietly disappeared is the failure mode this tablet has actually been hit by.
        added = sorted(set(shipped) - base)
        results.append((True, "基线之后的新增参数（提示，不拦）", ",".join(added)))

    # ---- 5. payload inputs: a silent swap of seed/addon/trust cache is a capability loss ----
    if base_doc.get("inputs"):
        for key, old in base_doc["inputs"].items():
            note(rec.get(key) == old, f"payload 输入 {key} 与基线一致", f"{str(rec.get(key))[:16]}")
    else:
        for key in ("trust_cache_sha256", "gpu_addon_sha256", "data_ramdisk_sha256"):
            note(key in rec, f"payload 输入 {key} 已记录", str(rec.get(key, ""))[:16])

    # ---- 6. no live source restored wholesale from one of its own .bak snapshots ----
    comparisons = 0
    identical = []
    for src in sorted((dev / "package").iterdir()):
        if src.suffix not in (".c", ".cpp", ".h", ".hpp"):
            continue
        for bak in sorted((dev / "package").glob(src.name + ".bak*")):
            comparisons += 1
            if bak.is_file() and bak.read_bytes() == src.read_bytes():
                identical.append(f"{src.name}=={bak.name}")
    note(comparisons > 0, "确实比对了源码与快照", f"{comparisons} 对")
    note(not identical, "没有文件与自己的 .bak 快照逐字节相同", ",".join(identical))

    # ---- 7. the HAL the iOS audio server loads is still the one from our repo ----
    hal = list((dev / "hal").glob("*")) if (dev / "hal").is_dir() else []
    note(bool([h for h in hal if h.suffix == ".c"]), "设备仓 hal/ 里有插件源码",
         ",".join(h.name for h in hal if h.suffix in (".c", ".h")))

    fails = [r for r in results if not r[0]]
    for ok, name, detail in results:
        if a.verbose or not ok:
            print(out_line(name, ok, detail))
    print(f"checks={len(results)} pass={len(results) - len(fails)} fail={len(fails)}")
    print("kernel_args_bytes=%d" % len(blob))
    if a.write_baseline:
        if fails:
            print("baseline NOT written: the artifact is red, pinning it would accept the regression")
            print("REGRESSION_GATE=1")
            return 1
        Path(a.baseline).write_text(json.dumps({
            "markers": markers,
            "args": shipped,
            "inputs": {k: rec[k] for k in ("trust_cache_sha256", "gpu_addon_sha256",
                                           "data_ramdisk_sha256", "adt_sha256") if k in rec},
            "kernel_sha256": ksha,
            "note": "accepted by device observation on 2026-11-08 (ord203): lock screen, charging "
                    "badge, touch, brightness, interface size, audio path all working",
        }, indent=2, ensure_ascii=False) + "\n")
        print("baseline written:", a.baseline)
    print("REGRESSION_GATE=%d" % (1 if fails else 0))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
