# Build Shim for RISC-V
## gnu-efi
1. clone upstream gnu-efi
```shell
cd /path/to/shimsrc
git clone https://git.code.sf.net/p/gnu-efi/code gnu-efi
```
2. compile RISC-V lib
```shell
cd gnu-efi
make ARCH=riscv64 CC=riscv64-linux-gnu-gcc LD=riscv64-linux-gnu-gcc HOSTCC=gcc
```
If cross-compile failed：
```shell
make ARCH=riscv64 \
      CC=riscv64-linux-gnu-gcc \
      HOSTCC=gcc \
      TOPDIR=$(pwd) \
      -f $(pwd)/Makefile \
      lib gnuefi inc
```


>编译RISCV库失败原因： gnu-efi的Make.defaults的第71行：
>`LD := $(prefix)$(CROSS_COMPILE)ld`
>
>gnu-efi 默认用 ld（裸链接器）做链接。而传入的 `LD=riscv64-linux-gnu-gcc`，把链接器改成了 gcc。gcc 不理解裸 ld 的 flags（--warn-common、--no-undefined 等），需要加 `-Wl,` 前缀
>
>与此同时，默认 make 构建全部四个子目录：
>
>`SUBDIRS = lib gnuefi inc apps # apps = 示例应用`
>
>apps 子目录的 Makefile 还会追加 `--defsym=EFI_SUBSYSTEM=$(SUBSYSTEM)`，进一步加剧问题

## Build Shim
1. create `crt0-efi-riscv64-local.o`
   Makefile of shim needs `crt0-efi-riscv64-local.o`,so:
   ```shell
   cp gnu-efi/riscv64/gnuefi/crt0-efi-riscv64.o \
      gnu-efi/riscv64/gnuefi/crt0-efi-riscv64-local.o
   ```
2. prepare cert
   ```shell
   openssl x509 -in DB.crt -outform DER -out /tmp/DB.der
   ```
3. build shim
   ```shell
   make -j$(nproc) \
    ARCH=riscv64 CROSS_COMPILE=riscv64-linux-gnu- \
    COMPILER=gcc ENABLE_SHIM_CERT=1 \
    VENDOR_CERT_FILE=/tmp/DB.der
   ```
4. build `BOOTRISCV64.CSV`
   ```shell
   echo "shimriscv64.efi,openEuler,,This is the boot entry for openEuler" | iconv -t UCS-2LE > BOOTRISCV64.CSV
   ```
5. build `.efi` (if `make` fail)
   ```shell
   riscv64-linux-gnu-objcopy -D \
    -j .text -j .sdata -j .data -j .data.ident \
    -j .dynamic -j .rodata -j '.rel*' -j '.rela*' \
    -j .dyn -j .reloc -j .eh_frame \
    -j .vendor_cert -j .sbat -j .sbatlevel \
    -O binary shimriscv64.so shimriscv64.efi
    ./post-process-pe shimriscv64.efi
   ```

## Fix Reloc !!!
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

## Sign Shim
```shell
sbsign --key DB.key --cert DB.crt --output shimriscv64.efi shimriscv64.efi
sbverify --cert DB.crt shimriscv64.efi
```

