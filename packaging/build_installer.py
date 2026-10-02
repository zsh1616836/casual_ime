"""Interactive Windows release build; uses only Python's standard library."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent


def parse_version(value):
    value = value.strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+(?:\.[0-9]+)?", value):
        raise argparse.ArgumentTypeError("版本号格式应为 1.1 或 1.1.2")
    if any(int(part) > 65535 for part in value.split(".")):
        raise argparse.ArgumentTypeError("版本号每一段不能超过 65535")
    return value


def find_iscc():
    override = os.environ.get("ISCC")
    if override:
        if Path(override).is_file():
            return str(Path(override).resolve())
        raise RuntimeError("ISCC 环境变量指向的文件不存在：" + override)
    command = shutil.which("ISCC.exe")
    if command:
        return command
    import winreg

    uninstall = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
    for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
            try:
                with winreg.OpenKey(hive, uninstall, 0, winreg.KEY_READ | view) as key:
                    for index in range(winreg.QueryInfoKey(key)[0]):
                        try:
                            with winreg.OpenKey(key, winreg.EnumKey(key, index)) as entry:
                                name = winreg.QueryValueEx(entry, "DisplayName")[0]
                                location = winreg.QueryValueEx(entry, "InstallLocation")[0]
                                candidate = Path(location) / "ISCC.exe"
                                if name.startswith("Inno Setup 7") and candidate.is_file():
                                    return str(candidate)
                        except OSError:
                            continue
            except OSError:
                continue
    raise RuntimeError("未找到 Inno Setup 7。请安装它，或设置 ISCC 为 ISCC.exe 的完整路径。")


def run(command, env):
    print("\n> " + subprocess.list2cmdline([str(part) for part in command]), flush=True)
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description="编译 x64/x86 并生成带版本号的安装包")
    parser.add_argument("--version", type=parse_version, help="例如 1.1 或 1.1.2；省略则交互输入")
    parser.add_argument("--configuration", choices=("Release", "RelWithDebInfo"), default="RelWithDebInfo")
    args = parser.parse_args()
    if os.name != "nt":
        raise RuntimeError("此脚本需要 Windows、Visual Studio C++ 构建工具和 Windows SDK。")

    version = args.version
    while version is None:
        try:
            version = parse_version(input("请输入安装包版本号（例如 1.1 或 1.1.2）："))
        except argparse.ArgumentTypeError as error:
            print(error)

    for command in ("cmake", "git"):
        if not shutil.which(command):
            raise RuntimeError("未找到 " + command + "，请安装并加入 PATH。")
    powershell = shutil.which("pwsh.exe") or shutil.which("powershell.exe")
    if not powershell:
        raise RuntimeError("未找到 PowerShell。")
    env = os.environ.copy()
    # Do not pass another PowerShell edition's module paths to the packager.
    for name in list(env):
        if name.lower() == "psmodulepath":
            del env[name]
    env["ISCC"] = find_iscc()

    config_path = ROOT / "ime" / "ime_config.h"
    original = config_path.read_bytes()
    updated, count = re.subn(
        rb'(#define[ \t]+IME_VERSION[ \t]+L")[^"\r\n]+(")',
        lambda match: match[1] + version.encode("ascii") + match[2],
        original,
    )
    if count != 1:
        raise RuntimeError("ime/ime_config.h 中应恰好有一处 IME_VERSION 定义。")

    print("\n工程：" + str(ROOT))
    print("版本：" + version + "；构建配置：" + args.configuration)
    # Preserve the header's BOM and line endings. Keep the requested version in
    # the source even after a failed build so the user can retry the same release.
    if updated != original:
        config_path.write_bytes(updated)
    version_path = ROOT / "packaging" / "version.txt"
    if version_path.read_text(encoding="utf-8-sig").strip() != version:
        version_path.write_bytes((version + "\r\n").encode("ascii"))

    for directory, architecture in (("build", "x64"), ("build32", "Win32")):
        cache = ROOT / directory / "CMakeCache.txt"
        command = ["cmake", "-S", str(ROOT), "-B", str(ROOT / directory)]
        if cache.exists():
            contents = cache.read_text(encoding="utf-8", errors="replace")
            if not re.search(r"^CMAKE_GENERATOR:INTERNAL=Visual Studio .+$", contents, re.M):
                raise RuntimeError(str(cache) + " 不是 Visual Studio 构建目录，请先移走该构建目录后重试。")
            platform = re.search(r"^CMAKE_GENERATOR_PLATFORM:INTERNAL=(.*)$", contents, re.M)
            if not platform or platform[1].lower() != architecture.lower():
                raise RuntimeError(str(cache) + " 的架构不匹配，应为 " + architecture)
            architecture = platform[1]
        command.extend(["-A", architecture, "-DZIME_PERF_DIAGNOSTIC=OFF"])
        run(command, env)

    run([
        powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
        str(ROOT / "packaging" / "build-installer.ps1"),
        "-Configuration", args.configuration,
    ], env)
    installer = ROOT / "dist" / ("casual_ime-" + version + "-x64.exe")
    if not installer.is_file():
        raise RuntimeError("打包脚本结束，但没有找到预期安装包：" + str(installer))
    print("\n安装包生成成功：" + str(installer), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KeyboardInterrupt, EOFError):
        print("\n已取消。", file=sys.stderr)
        sys.exit(130)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print("\n编译/打包失败：" + str(error), file=sys.stderr)
        sys.exit(1)
