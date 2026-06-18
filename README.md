# Build Shim for RISC-V
## gnu-efi
```
# clone upstream gnu-efi
cd /path/to/shimsrc
git clone https://git.code.sf.net/p/gnu-efi/code gnu-efi

# compile RISC-V lib
cd gnu-efi
make ARCH=riscv64 CC=riscv64-linux-gnu-gcc LD=riscv64-linux-gnu-gcc HOSTCC=gcc

# if cross-compile failed, try to only build three sub-directory we need.
make ARCH=riscv64 \
      CC=riscv64-linux-gnu-gcc \
      HOSTCC=gcc \
      TOPDIR=$(pwd) \
      -f $(pwd)/Makefile \
      lib gnuefi inc
```

编译RISCV库失败原因： gnu-efi的Make.defaults的第71行：
`LD := $(prefix)$(CROSS_COMPILE)ld`

gnu-efi 默认用 ld（裸链接器）做链接。而传入的 `LD=riscv64-linux-gnu-gcc`，把链接器改成了 gcc。gcc 不理解裸 ld 的 flags（--warn-common、--no-undefined 等），需要加 `-Wl,` 前缀

与此同时，默认 make 构建全部四个子目录：

`SUBDIRS = lib gnuefi inc apps # apps = 示例应用`

apps 子目录的 Makefile 还会追加 `--defsym=EFI_SUBSYSTEM=$(SUBSYSTEM)`，进一步加剧问题

## Build Shim
```
cd shim-src
# 如果 gnu-efi 还没 clone，直接 clone 到这里
# git clone https://git.code.sf.net/p/gnu-efi/code gnu-efi

# 创建 crt0-efi-riscv64-local.o (!important!)
cp gnu-efi/riscv64/gnuefi/crt0-efi-riscv64.o \
   gnu-efi/riscv64/gnuefi/crt0-efi-riscv64-local.o

# 转换 DB 证书为 DER
openssl x509 -in DB.crt -outform DER -out /tmp/DB.der

# 构建
make -j$(nproc) \
    ARCH=riscv64 CROSS_COMPILE=riscv64-linux-gnu- \
    COMPILER=gcc ENABLE_SHIM_CERT=1 \
    VENDOR_CERT_FILE=/tmp/DB.der

# 生成 BOOTRISCV64.CSV (shim 启动时读取，识别合法 bootloader)
# 默认 make 只生成 .efi，CSV 在 install-deps target 中，需单独生成
echo "shimriscv64.efi,openEuler,,This is the boot entry for openEuler" | iconv -t UCS-2LE > BOOTRISCV64.CSV

# 生成 .efi (如果 make 未完成)
riscv64-linux-gnu-objcopy -D \
    -j .text -j .sdata -j .data -j .data.ident \
    -j .dynamic -j .rodata -j '.rel*' -j '.rela*' \
    -j .dyn -j .reloc -j .eh_frame \
    -j .vendor_cert -j .sbat -j .sbatlevel \
    -O binary shimriscv64.so shimriscv64.efi
./post-process-pe shimriscv64.efi
```

### Fix Reloc !!(important)!!
gnu-efi CRT0 通过 `dummy - label1` 计算 `.reloc` 段的 Page RVA。 当 `.data VMA < .reloc VMA` 时，结果为负值 (如 0xFFFEB000)， UEFI PE 加载器拒绝此类映像，报 `Command Error Status: Unsupported`
执行：
```shell
python3 fix_reloc.py shimriscv64.efi  # Page RVA 负值 → 0x1000
```

`fix_reloc`的核心逻辑是：
```python
page_rva = struct.unpack_from('<I', d, raw_off)[0]
if page_rva >= 0x80000000:  # 负值 (unsigned > 2^31)
    struct.pack_into('<I', d, raw_off, 0x1000)  # 修正为有效值
```

### Sign Shim
```shell
sbsign --key DB.key --cert DB.crt --output shimriscv64.efi shimriscv64.efi
sbverify --cert DB.crt shimriscv64.efi
```

# shim, a first-stage UEFI bootloader

shim is a trivial EFI application that, when run, attempts to open and
execute another application. It will initially attempt to do this via the
standard EFI `LoadImage()` and `StartImage()` calls. If these fail (because Secure
Boot is enabled and the binary is not signed with an appropriate key, for
instance) it will then validate the binary against a built-in certificate. If
this succeeds and if the binary or signing key are not forbidden then shim
will relocate and execute the binary.

shim will also install a protocol which permits the second-stage bootloader
to perform similar binary validation. This protocol has a GUID as described
in the shim.h header file and provides a single entry point. On 64-bit systems
this entry point expects to be called with SysV ABI rather than MSABI, so calls
to it should not be wrapped.

On systems with a TPM chip enabled and supported by the system firmware,
shim will extend various PCRs with the digests of the targets it is
loading.  A full list is in the file [README.tpm](README.tpm) .

To use shim, simply place a DER-encoded public certificate in a file such as
pub.cer and build with `make VENDOR_CERT_FILE=pub.cer`.

There are a couple of build options, and a couple of ways to customize the
build, described in [BUILDING](BUILDING).

See the [test plan](testplan.txt), and file a ticket if anything fails!

In the event that the developers need to be contacted related to a security
incident or vulnerability, please mail [secalert@redhat.com].

[secalert@redhat.com]: mailto:secalert@redhat.com
