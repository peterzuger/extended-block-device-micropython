# extended-block-device-micropython

## Table of Contents
+ [About](#about)
+ [Getting Started](#getting_started)
+ [Usage](#usage)

## About <a name = "about"></a>
This a [Block
Device](https://docs.micropython.org/en/latest/library/vfs.html#block-devices)
Wrapper for [micropython](https://github.com/micropython/micropython) to enable
`pyb.MMC()` or `pyb.SDCard()` to be used with LittleFS.

## Getting Started <a name = "getting_started"></a>

### Prerequisites

```
git clone --recurse-submodules https://github.com/micropython/micropython.git
```

to compile the project, [make](https://www.gnu.org/software/make/),
[gcc](https://gcc.gnu.org/) and [arm-none-eabi-gcc](https://gcc.gnu.org/) is required,
install them from your package manager

### Installing
[extended-block-device-micropython](https://github.com/peterzuger/extended-block-device-micropython)
should work on any [micropython](https://github.com/micropython/micropython)
port.

First create a modules folder next to your copy of
[micropython](https://github.com/micropython/micropython) and put this project
in the modules folder.

```
cd modules
git clone https://gitlab.com/peterzuger/extended-block-device-micropython.git
```

```
project/
├── modules/
│   └──extended-block-device-micropython/
│       ├──...
│       └──micropython.mk
└── micropython/
    ├──ports/
   ... ├──stm32/
      ...
```

Now that all required changes are made, it is time to build [micropython](https://github.com/micropython/micropython),
for this cd to the top level directory of [micropython](https://github.com/micropython/micropython).
From here, first the mpy-cross compiler has to be built:
```
make -C mpy-cross
```

once this is built, compile your port with:
```
make -C ports/your port name here/ USER_C_MODULES=../modules CFLAGS_EXTRA=-DMODULE_EXTENDED_BLOCKDEV_ENABLED=1
```

and you are ready to use extended_blockdev.

## Usage <a name = "usage"></a>
The module is available by just importing extended_blockdev:
```
import extended_blockdev
import pyb
import vfs

bdev = extended_blockdev.EBDev(pyb.MMCard())

vfs.VfsLfs2.mkfs(bdev)
vfs.mount(bdev, "/disk")

os.chdir("/disk")
```

You can also specify an start and length address in bytes (must be a multiple of
the block_size of the underlying block device).

```
bdev = pyb.MMCard()

ebdev1 = extended_blockdev.EBDev(bdev, 0, 512 * 4096)
ebdev2 = extended_blockdev.EBDev(bdev, 512 * 4096)

vfs.VfsLfs2.mkfs(ebdev1)
vfs.VfsLfs2.mkfs(ebdev2)

vfs.mount(ebdev1, "/disk1")
vfs.mount(ebdev2, "/disk2")
```
