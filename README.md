# Keyless-Enhanced Secure Boot via OriginSign
By introducing the OriginSign keyless signing service during the secure boot phase, the restrictions imposed by MOK and the Microsoft DB on this process are reduced.

## Features
The Shim implements **keyless signature service** during the Secure Boot of the OS via [OriginSign]()

## Compatibly
Keyless-Shim is already compatible with x86 and RISCV.

## Branches
Three branches:
1. Shim15.5: The initial develop version, base shim-15.5
2. Shim15.7: Adapt [OpenEuler](https://atomgit.com/src-openeuler/shim) which based shim-15.7
3. RiscV: Complete the adaption to RISC-V.


## Acknowledgements
Official [Shim](https://github.com/rhboot/shim)

ISCAS-ISRC: Guanyu Liang

[AdjWang](https://github.com/AdjWang)

