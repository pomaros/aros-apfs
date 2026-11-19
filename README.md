# APFS for AROS

ApfsRead mounts an unlocked Apple File System volume as a temporary, read-only
AROS DOS volume. Use `List`, `Copy` and ordinary file reads on `APFSTEST:`.
A host build can inspect GPT disk images, list directories and extract files.

This project is part of John Knipper's series of native filesystem drivers for AROS.

| Filesystem | Origin | What it offers on AROS |
| --- | --- | --- |
| [AFS+](https://github.com/jonx/afsplus) | John Knipper designed this 64-bit filesystem from scratch and wrote its format specification. | B+ trees provide fast filename lookups. The allocator loads free-space maps a page at a time to keep memory use low. Transactions support recovery after power loss. |
| [exFAT](https://github.com/jonx/aros-extfat) | A clean-room driver written from scratch using Microsoft's published exFAT specification and the AROS DOS interfaces. | Reads, writes and formats removable media for exchange with other systems. Supports files larger than 4 GiB. |
| [APFS](https://github.com/pomaros/aros-apfs) | A native AROS DOS handler built on the libfsapfs reader for Apple's filesystem. | Mounts an unlocked volume read-only from a GPT disk or raw disk image. Browse directories with `List` and copy files out with `Copy`. |

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
the image digest is unchanged. Run `python3 tests/sealed.py` for a second fixture
created with Apple's `apfs_prepare_cryptex` tool. It checks 256 directory
entries and compares extracted file contents with the original bytes.
To generate a sealed GPT image for another reader, run
`python3 tests/sealed.py --make-image fixture.dmg`.

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
authorized hardware decryption.

The build patches libfsapfs to read headerless, hashed catalog nodes and the
physical file extent tree on sealed volumes. On an M1 running native AROS,
`List` enumerated the root of the internal Macintosh HD and `Copy` read
`System/Library/CoreServices/SystemVersion.plist` into RAM. The reader accesses
the current catalog. It does not authenticate Apple's signed seal or select a
boot snapshot.

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

Joe T. Sylve describes the catalog and file extent trees used by the reader in
[APFS Internals: Sealed Volumes](https://jtsylve.blog/post/2022/12/20/APFS-Sealed-Volumes).

Apple documents the on-disk structures and the relationship between containers
and volumes in the [Apple File System Reference](https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf).

## License

ApfsRead and its build and test tools use the [MIT License](LICENSE).
The downstream patch to libfsapfs uses the library's LGPL-3.0-or-later license.
The build links libfsapfs under LGPL-3.0-or-later. Its bundled dependencies
and zlib retain their respective licenses. The pinned source archive includes
the upstream copyright notices and license texts. [Upstream license information](https://github.com/libyal/libfsapfs#readme)
