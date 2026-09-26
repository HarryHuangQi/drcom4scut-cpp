# 预编译文件

`drcom4scut-ubuntu22.04-x86_64` 是最终源码在 Ubuntu 22.04.5 x86_64 用户空间中构建并通过集成测试的 ELF64 文件。

SHA-256：

```text
b623ce004b21d91ddc00ddc60021a7e75323cffd64c29802398e7e0b10a144be
```

建议优先使用安装脚本从源码编译。直接安装此文件：

```bash
sudo install -m 0755 bin/drcom4scut-ubuntu22.04-x86_64 /usr/local/bin/drcom4scut
```

运行依赖包括 Ubuntu 22.04 的 `libcrypto.so.3`、glibc、libstdc++ 和 libgcc。

Windows 10/11 x86_64 预编译文件：

- `drcom4scut.exe`：认证核心；SHA-256 `4f9f494a2ea35fb8b7d4991dd25be434b7489d02468e12f61a2f78f3a6a01868`
- `drcom4scut-gui.exe`：原生 GUI；SHA-256 `dc39d8dfd9bf08f486d06bc38b43d9e2023063a3097801a852d883751303d37b`

两个 EXE 必须放在同一目录。运行前安装 Npcap，配置和使用方法见 `docs/WINDOWS.md`。
