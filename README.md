# APFS for AROS

ApfsRead mounts an unlocked Apple File System volume as a temporary, read-only
AROS DOS volume. Use `List`, `Copy` and ordinary file reads on `APFSTEST:`.
A host build can inspect GPT disk images, list directories and extract files.

This project is part of John Knipper's series of native filesystem drivers for AROS.
[AFS+](https://github.com/jonx/afsplus) is a 64-bit filesystem with Unicode
filenames and transactions for recovery after a power loss.
The [exFAT handler](https://github.com/jonx/aros-extfat) reads, writes and
formats removable media for exchanging files with other operating systems.
The [APFS reader](https://github.com/pomaros/aros-apfs) mounts unlocked Apple
File System volumes read-only so you can browse directories and copy files
from a disk or a raw GPT image.

## Mount a volume

Start with a raw GPT disk image. The last argument below is its logical sector
size in bytes. ApfsRead supports 512 and 4096.

```text
ApfsRead --image Work:example.img 512
```

The inventory prints the partition number, volume index, volume UUID, lock
state and name. A partition may contain several APFS volumes. Select the
entry you want. For example, if the inventory reports partition 1 and volume 0,
run this in one AROS Shell:

```text
ApfsRead --image Work:example.img 512 1 0 --mount
```

After the mount message appears, use a second Shell. Substitute an existing
file on your volume for the example path.

```text
List APFSTEST:
Copy APFSTEST:Documents/example.txt RAM:example.txt
ApfsRead --unmount
```

Close files and leave the volume in any Shell using it as the current directory
before unmounting. ApfsRead serves one volume at a time. For direct disk
access, replace `--image Work:example.img 512` with `--device DEVICE UNIT`
using the AROS device name and unit for your GPT disk. The device must support
`TD_GETGEOMETRY` and `NSCMD_TD_READ64`.

## Build and test

The build downloads the pinned libfsapfs 20240429 source archive, verifies
its SHA-256, then compiles the library and its bundled libyal dependencies.
It requires a C compiler, make, curl, shasum and zlib development files.

```sh
tools/build host
build/host/ApfsRead --image example.img 512
build/host/ApfsRead --image example.img 512 1 0 /Documents
build/host/ApfsRead --image example.img 512 1 0 /Documents/example.txt copy.txt
```

On macOS, `python3 tests/image.py` creates a disposable APFS volume, compares
the volume UUID with Disk Utility, extracts a known file and verifies that
the image digest is unchanged.

For an AArch64 AROS build on Apple Silicon macOS, set these two paths to the
unpacked public SDK and the AROS LLVM installation. The executable is
`build/aros/ApfsRead`. [PomAROS AArch64 SDK](https://github.com/pomaros/aros-core/releases/tag/sdk-aarch64-9b25c24) · [AROS LLVM build instructions](https://github.com/pomaros/pomaros-hosted/blob/main/GETTING-STARTED.md)

```sh
export AROS_SDK=/path/to/PomAROS-SDK-aarch64-9b25c24
export AROS_TOOLCHAIN=/path/to/aros-crosstools
tools/build aros
```

## Scope

The reader targets regular files and directories on accessible APFS volumes
inside a valid primary GPT. Disk unlock, snapshot selection, symbolic link
traversal and macOS System/Data firmlink resolution are outside the current
mount implementation. The DOS mount uses classic file-info and seek packets.
Reading an encrypted internal Mac Data volume requires separate support for
authorized hardware decryption. Physical M1 volume access awaits testing.

## Acknowledgments and references

Thank you to Joachim Metz and the libyal contributors for publishing the
libraries that make this port possible, and to the AROS developers for the
DOS handler and device interfaces.

| Project | Contribution |
| --- | --- |
| [libyal/libfsapfs](https://github.com/libyal/libfsapfs) | The APFS reader linked by ApfsRead. It enumerates volumes, resolves directory entries and reads file data. |
| [libyal/libbfio](https://github.com/libyal/libbfio) | Its Basic File I/O interface connects the parser to a bounded, read-only partition. The build uses the copy bundled with libfsapfs. |
| [madler/zlib](https://github.com/madler/zlib) | Supplies GPT CRC-32 verification and zlib decompression for the reader. |
| [AROS](https://github.com/aros-development-team/AROS) and [PomAROS common core](https://github.com/pomaros/aros-core) | Provide the operating-system interfaces and public AArch64 SDK used by the port. |

Thanks also to the authors of the following independent readers for making
their work available for comparison:

- [sgan81/apfs-fuse](https://github.com/sgan81/apfs-fuse), a read-only userspace APFS driver.
- [linux-apfs/linux-apfs-rw](https://github.com/linux-apfs/linux-apfs-rw), an APFS filesystem module for Linux.
- [avighnac/libapfs](https://github.com/avighnac/libapfs), a C++ APFS reader.

Apple documents the on-disk structures and the relationship between containers
and volumes in the [Apple File System Reference](https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf).

## License

John Knipper's code in this repository is distributed under the [MIT License](LICENSE).
The build links libfsapfs under LGPL-3.0-or-later. Its bundled dependencies
and zlib retain their respective licenses. The pinned source archive includes
the upstream copyright notices and license texts. [Upstream license information](https://github.com/libyal/libfsapfs#readme)
