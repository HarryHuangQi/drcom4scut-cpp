# 预编译文件

`drcom4scut-ubuntu22.04-x86_64` 是最终源码在 Ubuntu 22.04.5 x86_64 用户空间中构建并通过集成测试的 ELF64 文件。

SHA-256：

```text
ed4a34dda99530370d58abd8b2a5d3e310c7c97bb9ed54bc3714bd98ba6c18fb
```

建议优先使用安装脚本从源码编译。直接安装此文件：

```bash
sudo install -m 0755 bin/drcom4scut-ubuntu22.04-x86_64 /usr/local/bin/drcom4scut
```

运行依赖包括 Ubuntu 22.04 的 `libcrypto.so.3`、glibc、libstdc++ 和 libgcc。
