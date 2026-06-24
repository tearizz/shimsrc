# shim — UEFI Secure Boot Bootloader with HTTP Remote Verification

本仓库是基于 [rhboot/shim](https://github.com/rhboot/shim) 的扩展版本，在保留原版 UEFI
Secure Boot 功能的基础上，新增了 **HTTP 远程验签 (KeyLess Signature Service)** 能力，
并支持 **x86_64** 与 **riscv64** 双架构编译。

---

## 目录

- [1. 与原版 rhboot/shim 的差异](#1-与原版-rhbootshim-的差异)
- [2. 功能详解](#2-功能详解)
  - [2.1 HTTP 远程验签](#21-http-远程验签-keyless-signature-service)
  - [2.2 独立 EFI 工具](#22-独立-efi-工具)
  - [2.3 网络驱动运行时加载](#23-网络驱动运行时加载-riscv64-特性)
  - [2.4 PKCS#7 签名解析](#24-pkcs7-签名解析)
- [3. 架构支持](#3-架构支持)
- [4. 编译指南](#4-编译指南)
  - [4.1 前置依赖](#41-前置依赖)
  - [4.2 gnu-efi 准备](#42-gnu-efi-准备)
  - [4.3 编译 riscv64 产物](#43-编译-riscv64-产物)
  - [4.4 编译 x86_64 产物](#44-编译-x86_64-产物)
  - [4.5 编译独立 EFI 工具](#45-编译独立-efi-工具)
- [5. gnu-efi 架构差异详解](#5-gnu-efi-架构差异详解)
- [6. 运行时环境](#6-运行时环境)
- [7. 验证流程数据流](#7-验证流程数据流)
- [8. 故障排除](#8-故障排除)

---

## 1. 与原版 rhboot/shim 的差异

### 新增文件

| 文件 | 行数 | 功能 |
|------|------|------|
| `keyless-sign.c` | ~790 | PKCS#7 签名提取、HTTP 验签请求、`osign_verify()`、`osign_parse_pkcs7()`、`osign_http_request()` |
| `keyless-sup.c` | ~150 | Base64 编码/解码 (`b64_encode`)、hex 转换 (`bin_to_hex_buf`)、X.509 DER 转换 |
| `keyless-sup.h` | ~15 | 辅助函数声明 |
| `keyless-stubs.c` | ~52 | 独立 `keyless-sign.efi` 工具的桩函数 (SBAT/TPM/secure_mode) |
| `http-request.c` | ~390 | HTTP GET/POST 请求、DHCP IP 获取 (`ip4_cfg2_get_data`)、网络驱动加载 |
| `http-request.h` | ~18 | HTTP 请求函数声明、全局变量声明 |
| `include/http.h` | ~500 | EFI HTTP 协议接口定义 (来自 TianoCore) |

### 修改文件

| 文件 | 修改内容 |
|------|----------|
| **`shim.c`** | ① `verify_buffer_authenticode()` 添加 HTTP 优先 + DB 兜底验签逻辑<br>② `shim_verify()` 增加 buffer 拷贝保护<br>③ `verify_buffer()` 调整为 authenticode → sbat 顺序<br>④ `read_image()` 增强 HTTP Boot 支持 |
| **`httpboot.c`** | ① 新增 `httpboot_fetch_buffer_uri()` — 通过指定 NIC handle + URI 获取 HTTP 资源<br>② `send_http_request()` 支持 POST 方法与 JSON body<br>③ 全局变量 `http_request_method` / `tx_body_json` 支持动态切换 GET/POST |
| **`shim.h`** | ① 增加 `AsciiSPrint()` 内联函数<br>② 移除 riscv64 `DEFAULT_LOADER` 宏（改为 Make.defaults 统一管理）<br>③ 添加 HTTP 相关 include |
| **`Makefile`** | ① OBJS 增加 `http-request.o keyless-sign.o keyless-sup.o`<br>② 新增 `http-request.efi` 独立构建目标<br>③ 新增 `keyless-sign.efi` 独立构建目标<br>④ 添加 `-DPAGE_SIZE=4096` 全局定义 |
| **`Make.defaults`** | ① 移除 riscv64 架构块（于 shim15.7 分支；riscv64 分支保留）<br>② x86_64 架构配置完整保留 |
| **`include/system/stdarg.h`** | x86_64 下分离 MS ABI 与 SysV ABI 的 `va_list` 类型定义 |
| **`netboot.c`** | `parseNetbootinfo()` / `extract_tftp_info()` 签名简化，移除 `str16_to_str8` 依赖 |
| **`pe.c`** | 内联 `ImageAddress()` / `relocate_coff()` 函数（原 `pe-relocate.c` 并入） |

---

## 2. 功能详解

### 2.1 HTTP 远程验签 (KeyLess Signature Service)

这是本仓库最核心的新增功能。在 UEFI Secure Boot 验证链中，shim 除了本地
DB/MokList 证书验证外，还会将 PE 二进制文件的 PKCS#7 签名数据发送到远程服务器
进行二次验证。

**验签策略 (HTTP 优先，本地 DB 兜底)：**

```
verify_buffer_authenticode()
  │
  ├─ ① 本地 DB/Shim/Vendor 证书签名循环验证
  │     └─ 结果保存在 ret_efi_status
  │
  ├─ ② HTTP 远程验证 (优先路径)
  │     ├─ osign_parse_pkcs7()    — 提取 payload/signature/certificate (Base64)
  │     ├─ osign_http_request()   — POST 到远程 /verify 端点
  │     ├─ 成功 → return EFI_SUCCESS (信任远程结果)
  │     └─ 失败 → dprint("falling back to DB")
  │
  └─ ③ 本地 DB 验证 (兜底路径)
        ├─ ret_efi_status == EFI_SUCCESS → 信任本地结果
        └─ 否则 → 验证失败, 拒绝启动
```

**HTTP 请求格式：**

```
POST http://10.0.2.2:8080/verify
Content-Type: application/json

{
  "certificate": "<Base64 DER-encoded X.509 certificate>",
  "payload":     "<Base64 DER-encoded SignedAttributes (SpcIndirectDataContent)>",
  "signature":   "<Base64 signature bytes>"
}
```

**关键函数调用链：**

```
shim_verify()                              # shim.c — EFI 安全协议入口
  └─ verify_buffer()                       # shim.c — 签名 + SBAT 验证
       └─ verify_buffer_authenticode()      # shim.c — 主验证逻辑
            ├─ check_denylist()             # 检查 dbx 黑名单
            ├─ check_allowlist()            # 检查 db/MokList 白名单
            ├─ verify_one_signature() x N   # 逐签名验证
            ├─ osign_parse_pkcs7()          # keyless-sign.c — PKCS#7 解析
            │    ├─ generate_hash()         # 计算 SHA-256/SHA-1
            │    ├─ d2i_PKCS7()            # OpenSSL ASN.1 解码 PKCS#7
            │    ├─ extract_verification_data()
            │    │    ├─ 验证 messageDigest
            │    │    ├─ 验证 DigestInfo (文件哈希比对)
            │    │    ├─ b64_encode(SignedAttributes)  → payload
            │    │    ├─ b64_encode(signature)         → signature
            │    │    └─ i2d_X509 + b64_encode         → certificate
            │    └─ 返回 payload/signature/certificate
            └─ osign_http_request()         # keyless-sign.c — HTTP 远程验签
                 ├─ AsciiSPrint()           # 构建 JSON body
                 └─ send_http_get_request() # http-request.c — HTTP 传输层
                      ├─ load_network_drivers()
                      ├─ BS->LocateHandleBuffer(EFI_HTTP_BINDING_GUID)
                      ├─ DHCP 获取 IP (Ip4Config2PolicyDhcp)
                      └─ httpboot_fetch_buffer_uri()
                           └─ http_fetch()
                                ├─ configure_http()
                                ├─ send_http_request()  # POST with JSON
                                └─ receive_http_response()
```

### 2.2 独立 EFI 工具

除了集成在 shim 内部的验签逻辑，我们还提供了两个独立的 EFI 可执行文件用于测试：

**`keyless-sign.efi`** — 独立的签名提取与验证工具

```bash
make keyless-sign.efi ARCH=<arch>
```

读取 EFI 文件 → 提取 PKCS#7 签名 → Base64 编码 → 发送到远程验证服务器。
使用 `keyless-stubs.c` 中的桩函数代替完整的 shim 启动逻辑。

**`http-request.efi`** — HTTP 请求测试工具

```bash
make http-request.efi ARCH=<arch>
```

测试底层 HTTP GET/POST 请求功能，包括 DHCP IP 获取和网络驱动加载。

### 2.3 网络驱动运行时加载 (riscv64 特性)

`http-request.c` 中的 `load_network_drivers()` 函数在运行时从 ESP 分区加载
网络驱动栈：

```
\EFI\BOOT\Hash2DxeCrypto.efi   — 哈希加密驱动 (TcpDxe 依赖)
\EFI\BOOT\TcpDxe.efi           — TCP 协议驱动
\EFI\BOOT\HttpDxe.efi          — HTTP 协议驱动
```

加载后等待最多 30 秒 (每秒轮询一次) 直到 `EFI_HTTP_BINDING_GUID` 协议可用。

> **注意**：x86_64 的 OVMF/EDK2 固件通常已内置完整网络栈，此步骤会自动跳过
> （驱动已存在时 `LoadImage` 返回错误，循环体直接 `continue`）。

### 2.4 PKCS#7 签名解析

`osign_parse_pkcs7()` 实现了完整的 Authenticode 签名提取：

1. **定位 Security Directory**：从 PE 头的 `SecDir->VirtualAddress` 找到签名数据
2. **定位最后一个 PKCS#7 记录**：遍历 Security Directory 找到 `WIN_CERT_TYPE_PKCS_SIGNED_DATA` 记录
3. **OpenSSL d2i_PKCS7()**：ASN.1 DER 解码
4. **验证 messageDigest**：比对 `SpcIndirectDataContent` 的哈希值与 SignedAttributes 中的 `messageDigest`
5. **验证 DigestInfo**：比对 `SpcIndirectDataContent` 内的文件哈希值与 `generate_hash()` 的结果
6. **提取三方数据**：
   - `payload` = SignedAttributes DER 的 Base64 (待签名的实际数据)
   - `signature` = PKCS#7 signerInfo.enc_digest 的 Base64
   - `certificate` = X.509 签名证书 DER 的 Base64

---

## 3. 架构支持

| 架构 | 状态 | EFI 产物名 | 验证方式 |
|------|------|-----------|----------|
| **x86_64** | ✅ 支持 | `shimx64.efi` | QEMU + OVMF |
| **riscv64** | ✅ 支持 | `shimriscv64.efi` | QEMU + RISC-V EDK2 |

**shim C 源码的架构适配策略：**

shim 的 C 代码是**架构无关的**（约 99% 代码无需修改）。极少数架构相关代码
（约 50 行）集中在以下位置，均为硬件抽象层：

| 文件 | 架构相关代码 | 作用 |
|------|-------------|------|
| `include/asm.h` | `rdtsc` / `mrs pmccntr_el0` / `msleep` | 时间戳读取、调试断点 |
| `shim.h` | `GNU_EFI_USE_MS_ABI` / `DEFAULT_LOADER` | x86_64 调用约定、默认引导器名 |
| `shim.c` | `debug_hook` 循环次数 | 调试断点等待上限 |
| `pe.c` | `IMAGE_FILE_MACHINE_X64` 校验 | PE 机器类型验证 |
| `include/system/stdarg.h` | `__builtin_ms_va_list` vs `__builtin_va_list` | 变参类型定义 |

**架构差异的实质**在于三个层面：

1. **编译器与工具链**：`gcc` (x86_64) vs `riscv64-linux-gnu-gcc` (交叉编译)
2. **gnu-efi 库**：`lib/x86_64/` (含 `callwrap.c`、`efi_stub.S`) vs `lib/riscv64/`
3. **编译/链接参数**：`--target efi-app-x86_64` (COFF) vs `-O binary` (原始二进制)

---

## 4. 编译指南

### 4.1 前置依赖

```bash
# 所有架构通用依赖 (Ubuntu/Debian)
apt-get install -y \
    gcc make git \
    libelf-dev \
    openssl \
    dos2unix \
    sbsigntool

# riscv64 交叉编译额外依赖
apt-get install -y \
    gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu
```

### 4.2 gnu-efi 准备

gnu-efi 是为每个架构**独立编译**的底层 UEFI 库。

#### 4.2.1 获取 gnu-efi 源码

```bash
cd /path/to/shimsrc

# 方式一: 使用项目自带的子模块 (推荐)
git submodule update --init --recursive

# 方式二: 独立 clone (版本: osignRV 分支)
git clone https://git.code.sf.net/p/gnu-efi/code gnu-efi
cd gnu-efi
git checkout osignRV
cd ..
```

#### 4.2.2 编译 gnu-efi for riscv64

```bash
cd gnu-efi

# 标准编译 (仅 lib + gnuefi + inc, 不编译 apps)
make ARCH=riscv64 \
    CC=riscv64-linux-gnu-gcc \
    HOSTCC=gcc \
    TOPDIR=$(pwd) \
    -f $(pwd)/Makefile \
    lib gnuefi inc

# 如果交叉编译失败，显式指定 LD
make ARCH=riscv64 \
    CC=riscv64-linux-gnu-gcc \
    LD=riscv64-linux-gnu-ld \
    HOSTCC=gcc \
    TOPDIR=$(pwd) \
    -f $(pwd)/Makefile \
    lib gnuefi inc

cd ..
```

> **关于编译失败**：gnu-efi 的 `Make.defaults` 第 71 行默认 `LD := $(CROSS_COMPILE)ld`。
> 如果你传入 `LD=riscv64-linux-gnu-gcc`，gcc 不理解裸 ld 的 flags
> (`--warn-common`、`--no-undefined` 等)，需要 `-Wl,` 前缀。
> 解决方法是**不传 LD** 或**传 `LD=riscv64-linux-gnu-ld`**。

#### 4.2.3 编译 gnu-efi for x86_64

```bash
cd gnu-efi

# x86_64 使用原生编译器，最简单
make ARCH=x86_64 lib gnuefi inc

# 或完整编译
make ARCH=x86_64

cd ..
```

#### 4.2.4 gnu-efi 编译后验证

```bash
# riscv64
ls gnu-efi/riscv64/lib/libefi.a
ls gnu-efi/riscv64/gnuefi/libgnuefi.a
ls gnu-efi/riscv64/gnuefi/crt0-efi-riscv64.o

# x86_64
ls gnu-efi/x86_64/lib/libefi.a
ls gnu-efi/x86_64/gnuefi/libgnuefi.a
ls gnu-efi/x86_64/gnuefi/crt0-efi-x86_64.o
```

### 4.3 编译 riscv64 产物

#### 4.3.1 准备 crt0 (riscv64 特有)

shim 的 Makefile 需要 `crt0-efi-riscv64-local.o`：

```bash
cp gnu-efi/riscv64/gnuefi/crt0-efi-riscv64.o \
   gnu-efi/riscv64/gnuefi/crt0-efi-riscv64-local.o
```

#### 4.3.2 准备签名证书

```bash
# 将 PEM 格式证书转为 DER (shim 要求 DER 格式)
openssl x509 -in your-vendor-cert.crt -outform DER -out /tmp/DB.der
```

#### 4.3.3 编译

```bash
make -j$(nproc) \
    ARCH=riscv64 \
    CROSS_COMPILE=riscv64-linux-gnu- \
    COMPILER=gcc \
    ENABLE_SHIM_CERT=1 \
    VENDOR_CERT_FILE=/tmp/DB.der
```

#### 4.3.4 修复 .reloc 段 Page RVA (riscv64 特有)

gnu-efi CRT0 通过 `dummy - label1` 计算 `.reloc` 段的 Page RVA。
当 `.data VMA < .reloc VMA` 时，计算结果为负值（如 `0xFFFEB000`），
UEFI PE 加载器拒绝加载，报 `Command Error Status: Unsupported`。

**必须执行修复：**

```python
# fix_reloc.py — 将负值 Page RVA 修正为 0x1000
import struct, sys

with open(sys.argv[1], 'r+b') as f:
    d = bytearray(f.read())

# 定位 .reloc 段
for off in range(0, len(d) - 512, 4):
    if d[off:off+4] == b'.reloc':
        # 读取 Page RVA (段数据起始偏移 +4 处)
        raw_off = off + 4 + 20  # section header → data offset
        page_rva = struct.unpack_from('<I', d, raw_off)[0]
        if page_rva >= 0x80000000:  # 负值 (unsigned > 2^31)
            struct.pack_into('<I', d, raw_off, 0x1000)
            print(f"Fixed Page RVA: 0x{page_rva:08X} → 0x00001000")
            break

with open(sys.argv[1], 'wb') as f:
    f.write(d)
```

```bash
python3 fix_reloc.py shimriscv64.efi
```

#### 4.3.5 签名

```bash
sbsign --key DB.key --cert DB.crt --output shimriscv64.efi shimriscv64.efi
sbverify --cert DB.crt shimriscv64.efi
```

#### 4.3.6 可选: 手动 objcopy

如果 `make` 的 objcopy 步骤失败，可以手动执行：

```bash
riscv64-linux-gnu-objcopy -D \
    -j .text -j .sdata -j .data -j .data.ident \
    -j .dynamic -j .rodata -j '.rel*' -j '.rela*' \
    -j .dyn -j .reloc -j .eh_frame \
    -j .vendor_cert -j .sbat -j .sbatlevel \
    -O binary shimriscv64.so shimriscv64.efi
./post-process-pe shimriscv64.efi
```

### 4.4 编译 x86_64 产物

#### 4.4.1 编译

x86_64 使用原生编译器，流程简单很多：

```bash
# 清理之前可能存在的其他架构产物
make clean

# 编译 (ARCH 自动检测为 x86_64)
make -j$(nproc) \
    ENABLE_SHIM_CERT=1 \
    VENDOR_CERT_FILE=/path/to/your/cert.der

# 或者显式指定架构
make -j$(nproc) \
    ARCH=x86_64 \
    ENABLE_SHIM_CERT=1 \
    VENDOR_CERT_FILE=/path/to/your/cert.der
```

#### 4.4.2 产物

```bash
# x86_64 编译产物
shimx64.efi          # 主 shim 引导程序 (~970 KB)
mmx64.efi            # MokManager (~860 KB)
fbx64.efi            # Fallback (~98 KB)
shimx64.efi.debug    # 调试符号
```

#### 4.4.3 x86_64 不需要的步骤

- ❌ 不需要 `crt0-efi-riscv64-local.o` 复制
- ❌ 不需要 `fix_reloc.py` Page RVA 修复
- ❌ 不需要交叉编译工具链

### 4.5 编译独立 EFI 工具

```bash
# HTTP 请求测试工具
make http-request.efi ARCH=<x86_64|riscv64>

# 密钥签名验证工具
make keyless-sign.efi ARCH=<x86_64|riscv64>
```

---

## 5. gnu-efi 架构差异详解

gnu-efi 是 shim 与 UEFI 固件之间的**翻译层**，每个架构有独立的实现目录：

```
gnu-efi/
├── inc/                          # 头文件
│   ├── x86_64/
│   │   ├── efibind.h             # MS ABI 调用约定, UEFI 类型映射
│   │   ├── efilibplat.h          # 平台特定库函数
│   │   ├── efisetjmp_arch.h      # setjmp/longjmp 汇编
│   │   └── pe.h                  # PE/COFF 格式定义 (x86 特有)
│   └── riscv64/
│       ├── efibind.h             # EFI 类型映射 (无 MS ABI)
│       ├── efilibplat.h
│       └── efisetjmp_arch.h
│
├── lib/x86_64/
│   ├── callwrap.c                # MS ABI ↔ SysV ABI 调用转换 (x86 特有!)
│   ├── efi_stub.S                # EFI 入口汇编 (x86 特有!)
│   ├── initplat.c                # 平台初始化
│   ├── math.c                    # 整数除法和取模
│   └── setjmp.S                  # setjmp 汇编 (x86)
└── lib/riscv64/
    ├── initplat.c                # 平台初始化 (riscv64)
    ├── math.c
    └── setjmp.S                  # setjmp 汇编 (riscv64)
```

### 关键差异

| 维度 | x86_64 | riscv64 |
|------|--------|---------|
| **调用约定** | Microsoft x64 (MS ABI) | 标准 C (SysV-like) |
| **ABI 桥接** | 需要 `callwrap.c` 做 MS↔SysV 转换 | 不需要 |
| **EFI 入口** | `efi_stub.S` 汇编入口 | CRT0 C 入口 |
| **可执行格式** | PE/COFF (`--target efi-app-x86_64`) | 原始二进制 (`-O binary`) |
| **子系统** | 隐式 (PE header) | 显式 `--defsym=EFI_SUBSYSTEM=0xa` |
| **CRT0 对象** | `crt0-efi-x86_64.o` (标准) | `crt0-efi-riscv64-local.o` (修复版) |
| **va_list 类型** | `__builtin_ms_va_list` (定制 stdarg.h) | `__builtin_va_list` |
| **额外链接标志** | `-mno-red-zone -mno-mmx -mno-sse` | `-mno-strict-align` |

### 为什么 gnu-efi 必须按架构分别编译

gnu-efi 库包含汇编代码（入口点、setjmp、调用转换），这些**不能跨架构使用**。
每个架构的汇编语法、寄存器名称、ABI 约定都不同：

- `lib/x86_64/efi_stub.S` — x86_64 汇编（`movq %rcx, image_handle` 等）
- `lib/riscv64/setjmp.S` — RISC-V 汇编（`sd ra, 0(a0)` 等）

当你执行 `make ARCH=x86_64` 时，gnu-efi 自动选择 `lib/x86_64/` 下的源文件
编译。切换 `ARCH=riscv64` 时同理。

**shim 本身不需要任何代码修改**，因为 UEFI 抽象层已经通过 gnu-efi 头文件
（`efi.h`, `efilib.h`）屏蔽了架构差异。你在 shim 中调用的 `BS->HandleProtocol()`
等函数，实际调用链为：

```
shim 代码: BS->HandleProtocol(...)
    │
    ▼
gnu-efi 头文件宏: uefi_call_wrapper(BS->HandleProtocol, ...)
    │
    ├─ x86_64: 展开为直接函数调用 (MS ABI 兼容)
    └─ riscv64: 展开为 efi_callX() trampoline
```

---

## 6. 运行时环境

### 6.1 远程验签服务器

HTTP 远程验签需要一个运行在宿主机的验证服务：

- **地址**：`http://10.0.2.2:8080/verify`（QEMU 默认宿主地址）
- **方法**：POST
- **Content-Type**：`application/json`
- **请求体**：
  ```json
  {
    "certificate": "<Base64 DER X.509>",
    "payload": "<Base64 DER SignedAttributes>",
    "signature": "<Base64 signature>"
  }
  ```

> **注意**：`10.0.2.2` 是 QEMU 用户模式网络的特殊地址，代表宿主机。
> 在真实硬件上，需要修改 `keyless-sign.c` 中 `osign_http_request()` 的
> `uri_literal` 变量为实际服务器地址。

### 6.2 UEFI 固件要求

| 架构 | 推荐固件 | 网络栈 |
|------|---------|--------|
| x86_64 | OVMF (edk2) | 内置 HTTP/TCP/DHCP 驱动 |
| riscv64 | RISC-V EDK2 | 可能需要从 ESP 加载 `Hash2DxeCrypto.efi`, `TcpDxe.efi`, `HttpDxe.efi` |

### 6.3 Shim 锁协议

shim 通过 `EFI_SHIM_LOCK_GUID` 协议向后续引导阶段（GRUB、MokManager）暴露服务：

| 服务 | 函数 | 作用 |
|------|------|------|
| Verify | `shim_verify()` | 安全启动验证（含 HTTP 远程验签） |
| Hash | `shim_hash()` | PE 哈希计算 |
| Context | `shim_read_header()` | PE 头部解析 |

---

## 7. 验证流程数据流

```
UEFI 固件
    │
    ▼
shimx64.efi / shimriscv64.efi
    │
    ├─ efi_main()
    │     ├─ InitializeLib()            # gnu-efi 初始化
    │     ├─ init_openssl()             # OpenSSL 初始化 (Cryptlib 嵌入式)
    │     ├─ import_mok_state()         # MOK 密钥导入
    │     ├─ shim_init()
    │     │     ├─ set_second_stage()   # 解析引导选项
    │     │     ├─ hook_system_services()
    │     │     └─ install_shim_protocols()
    │     └─ init_grub()
    │           └─ start_image()
    │                 └─ read_image() + handle_image()
    │                       └─ shim_verify()
    │                             │
    │                             ├─ 复制 buffer (保护调用者内存)
    │                             ├─ read_header()
    │                             ├─ verify_buffer()
    │                             │     ├─ verify_buffer_authenticode()
    │                             │     │     ├─ [本地] check_denylist()
    │                             │     │     ├─ [本地] check_allowlist()
    │                             │     │     ├─ [本地] verify_one_signature() x N
    │                             │     │     ├─ [远程] osign_parse_pkcs7()
    │                             │     │     │     └─ 提取 payload/sig/cert
    │                             │     │     └─ [远程] osign_http_request()
    │                             │     │           └─ HTTP POST → 服务器
    │                             │     └─ verify_buffer_sbat()
    │                             └─ 释放 buffer_copy
    │
    ▼
grubx64.efi / grubriscv64.efi
    │
    ▼
Linux Kernel
```

---

## 8. 故障排除

### 8.1 gnu-efi 编译失败

**问题**：`riscv64-linux-gnu-gcc: error: unrecognized command line option`

**解决**：不要传 `LD=riscv64-linux-gnu-gcc`，使用裸 `ld` 或 `LD=riscv64-linux-gnu-ld`。

```bash
make ARCH=riscv64 \
    CC=riscv64-linux-gnu-gcc \
    HOSTCC=gcc \
    TOPDIR=$(pwd) -f $(pwd)/Makefile \
    lib gnuefi inc
```

### 8.2 shim 编译失败: va_list 未定义 (x86_64)

**问题**：
```
efilib.h:541:5: error: unknown type name 'va_list'
```

**原因**：`include/system/stdarg.h` 在 x86_64 的 `__x86_64__` 分支中定义了
`ms_va_list` 和 `sysv_va_list`，但遗漏了 `va_list` 的定义。

**解决**：在 `include/system/stdarg.h` 的 `__x86_64__` 分支中确认存在：

```c
#elif defined(__x86_64__)
typedef __builtin_ms_va_list ms_va_list;
#define ms_va_copy(dest, start)  __builtin_ms_va_copy(dest, start)
// ...
typedef __builtin_ms_va_list va_list;       // ← 这一行
#define va_start(v,l)  __builtin_ms_va_start(v,l)  // ← 这些也必须存在
#define va_end(v)      __builtin_ms_va_end(v)
```

### 8.3 AsciiSPrint 未定义

**问题**：
```
implicit declaration of function 'AsciiSPrint'
```

**解决**：确保 `shim.h` 中有这个内联函数（riscv64 分支已包含）：

```c
#ifndef SHIM_UNIT_TEST
static inline UINTN
AsciiSPrint(CHAR8 *buf, UINTN buf_size, const CHAR8 *fmt, ...)
{
    va_list args;
    UINTN len;
    va_start(args, fmt);
    len = AsciiVSPrint(buf, buf_size, fmt, args);
    va_end(args);
    return len;
}
#endif
```

### 8.4 riscv64 .reloc Page RVA 负值

**问题**：UEFI 固件报 `Command Error Status: Unsupported`

**原因**：gnu-efi CRT0 计算的 `.reloc` Page RVA 为负值。

**解决**：运行 `fix_reloc.py`（见 [4.3.4](#434-修复-reloc-段-page-rva-riscv64-特有)）。

### 8.5 远程验签服务不可达

- QEMU 环境：确保验证服务监听在 `10.0.2.2:8080`
- 真实硬件：修改 `keyless-sign.c` 中的 `uri_literal` 为实际服务地址
- HTTP 不可达时，shim 会回退到本地 DB 验证，不会阻止启动
- 查看调试输出：设置 `SHIM_VERBOSE` UEFI 变量观察 `dprint()` 输出

### 8.6 HTTP binding 超时 (riscv64)

**问题**：`Timeout waiting for HTTP binding`

**原因**：RISC-V EDK2 固件缺少网络驱动或驱动未正确加载。

**排查**：
1. 确认 `\EFI\BOOT\` 下存在 `Hash2DxeCrypto.efi`, `TcpDxe.efi`, `HttpDxe.efi`
2. 检查 UEFI 固件版本是否支持 HTTP 协议
3. 尝试手动在 UEFI Shell 中加载驱动：
   ```
   Shell> load Hash2DxeCrypto.efi
   Shell> load TcpDxe.efi
   Shell> load HttpDxe.efi
   ```
