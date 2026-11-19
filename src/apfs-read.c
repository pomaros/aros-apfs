// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
#include "disk.h"
#ifdef __AROS__
#include "mount.h"
#endif
#include <libfsapfs.h>
#include "libfsapfs_container.h"
#include "libbfio_handle.h"
#include "libbfio_definitions.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

struct slice {
    struct disk *disk;
    uint64_t start, size, pos;
};

static int always_open(intptr_t *opaque, libcerror_error_t **err)
{
    (void)opaque; (void)err;
    return 1;
}

static int slice_size(intptr_t *opaque, size64_t *size, libcerror_error_t **err)
{
    (void)err;
    *size = ((struct slice *)opaque)->size;
    return 1;
}

static off64_t slice_seek(intptr_t *opaque, off64_t offset, int whence,
                          libcerror_error_t **err)
{
    struct slice *s = (struct slice *)opaque;
    uint64_t base;
    (void)err;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = s->pos; break;
    case SEEK_END: base = s->size; break;
    default: return -1;
    }
    if ((offset < 0 && (uint64_t)(-(offset + 1)) >= base) ||
        (offset >= 0 && (uint64_t)offset > s->size - base)) return -1;
    s->pos = base + (uint64_t)offset;
    return (off64_t)s->pos;
}

static ssize_t slice_read(intptr_t *opaque, uint8_t *out, size_t n,
                          libcerror_error_t **err)
{
    struct slice *s = (struct slice *)opaque;
    (void)err;
    if (n > s->size - s->pos) n = (size_t)(s->size - s->pos);
    if (n > SSIZE_MAX || !disk_read(s->disk, s->start + s->pos, out, n)) return -1;
    s->pos += n;
    return (ssize_t)n;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static void print_uuid(const uint8_t *id)
{
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) putchar('-');
        printf("%02x", id[i]);
    }
}

static int list_dir(libfsapfs_file_entry_t *dir, libfsapfs_error_t **err)
{
    int n;
    if (libfsapfs_file_entry_get_number_of_sub_file_entries(dir, &n, err) != 1)
        return 0;
    for (int i = 0; i < n; i++) {
        libfsapfs_file_entry_t *child = NULL;
        uint8_t name[1024];
        int ok = libfsapfs_file_entry_get_sub_file_entry_by_index(dir, i, &child, err) == 1 &&
            libfsapfs_file_entry_get_utf8_name(child, name, sizeof(name), err) == 1;
        libfsapfs_file_entry_free(&child, NULL);
        if (!ok) return 0;
        puts((char *)name);
    }
    return 1;
}

static int copy_file(libfsapfs_file_entry_t *file, const char *path, libfsapfs_error_t **err)
{
    uint8_t *buf;
    size64_t size;
    if (libfsapfs_file_entry_get_size(file, &size, err) != 1 || size > INT64_MAX)
        return 0;
#ifdef __AROS__
    if (strncmp(path, "RAM:", 4) || !path[4] || strpbrk(path + 4, ":/")) {
        fprintf(stderr, "Choose a new RAM:filename for the copy.\n");
        return 0;
    }
#endif
    buf = malloc(65536);
    if (!buf) return 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror(path); free(buf); return 0; }
    uint64_t at = 0;
    while (at < size) {
        size_t n = size - at > 65536 ? 65536 : (size_t)(size - at);
        if (libfsapfs_file_entry_read_buffer_at_offset(file, buf, n, at, err) != (ssize_t)n ||
            write(fd, buf, n) != (ssize_t)n) break;
        at += n;
    }
    int closed = close(fd);
    free(buf);
    if (at != size || closed != 0) {
        unlink(path);
        return 0;
    }
    printf("copied %llu bytes\n", (unsigned long long)size);
    return 1;
}

static int visit(struct slice *s, int part, int wanted, const char *path, const char *dest)
{
    libbfio_handle_t *io = NULL;
    libfsapfs_container_t *container = NULL;
    libfsapfs_volume_t *volume = NULL;
    libfsapfs_file_entry_t *entry = NULL;
    libfsapfs_error_t *err = NULL;
    int n = 0, ok = 0;
    if (libbfio_handle_initialize(&io, (intptr_t *)s, NULL, NULL, NULL, NULL,
            slice_read, NULL, slice_seek, always_open, always_open, slice_size,
            LIBBFIO_FLAG_IO_HANDLE_NON_MANAGED, &err) != 1 ||
        libfsapfs_container_initialize(&container, &err) != 1 ||
        libfsapfs_container_open_file_io_handle(container, io, LIBBFIO_OPEN_READ, &err) != 1 ||
        libfsapfs_container_get_number_of_volumes(container, &n, &err) != 1) goto out;
    if (wanted >= n) { fprintf(stderr, "Volume index out of range.\n"); goto out; }
    int all_ok = 1;
    for (int i = 0; i < n; i++) {
        uint8_t name[1024], id[16];
        if (wanted >= 0 && i != wanted) continue;
        if (libfsapfs_container_get_volume_by_index(container, i, &volume, &err) != 1 ||
            libfsapfs_volume_get_identifier(volume, id, sizeof(id), &err) != 1 ||
            libfsapfs_volume_get_utf8_name(volume, name, sizeof(name), &err) != 1) {
            fprintf(stderr, "Cannot inspect partition %d volume %d.\n", part, i);
            if (err) libfsapfs_error_backtrace_fprint(err, stderr);
            libfsapfs_error_free(&err);
            libfsapfs_volume_free(&volume, NULL);
            all_ok = 0;
            continue;
        }
        int locked = libfsapfs_volume_is_locked(volume, &err);
        if (locked < 0) goto out;
        printf("partition=%d volume=%d uuid=", part, i);
        print_uuid(id);
        printf(" locked=%d name=%s\n", locked, name);
        if (path) {
            if (locked) { fprintf(stderr, "Volume locked.\n"); goto out; }
#ifdef __AROS__
            if (!strcmp(path, "--mount")) {
                ok = apfs_serve(volume);
                goto out;
            }
#endif
            if (libfsapfs_volume_get_file_entry_by_utf8_path(volume, (const uint8_t *)path,
                    strlen(path), &entry, &err) != 1) {
                fprintf(stderr, "Cannot resolve %s\n", path);
                goto out;
            }
            uint16_t mode;
            if (libfsapfs_file_entry_get_file_mode(entry, &mode, &err) != 1) goto out;
            if ((mode & 0170000) != (dest ? 0100000 : 0040000)) {
                fprintf(stderr, "Unexpected file type for %s\n", path);
                goto out;
            }
            if (!(dest ? copy_file(entry, dest, &err) : list_dir(entry, &err))) goto out;
        }
        libfsapfs_file_entry_free(&entry, NULL);
        libfsapfs_volume_free(&volume, NULL);
    }
    ok = all_ok;
out:
    if (err) libfsapfs_error_backtrace_fprint(err, stderr);
    libfsapfs_error_free(&err);
    libfsapfs_file_entry_free(&entry, NULL);
    libfsapfs_volume_free(&volume, NULL);
    libfsapfs_container_free(&container, NULL);
    libbfio_handle_free(&io, NULL);
    return ok;
}

static int scan(struct disk *disk, int part, int volume, const char *path, const char *dest)
{
    unsigned sector = disk_sector(disk);
    uint64_t sectors = disk_length(disk) / sector;
    uint8_t header[4096];
    if (!disk_read(disk, sector, header, sector) || memcmp(header, "EFI PART", 8)) {
        fprintf(stderr, "Cannot read primary GPT.\n");
        return 0;
    }
    uint32_t hsize = le32(header + 12), hcrc = le32(header + 16);
    memset(header + 16, 0, 4);
    uint64_t first = le64(header + 40), last = le64(header + 48), table = le64(header + 72);
    uint32_t count = le32(header + 80), stride = le32(header + 84);
    uint64_t bytes = (uint64_t)count * stride;
    if (hsize < 92 || hsize > sector || crc32(0, header, hsize) != hcrc ||
        le64(header + 24) != 1 || first > last || last >= sectors ||
        table < 2 || table >= first || !count || stride < 128 || stride % 8 ||
        bytes > 4 * 1024 * 1024 || bytes > (first - table) * sector) {
        fprintf(stderr, "Invalid primary GPT header.\n");
        return 0;
    }
    uint8_t *entries = malloc(bytes);
    int ok = 1, found = 0;
    if (!entries || !disk_read(disk, table * sector, entries, bytes) ||
        crc32(0, entries, bytes) != le32(header + 88)) {
        fprintf(stderr, "Cannot verify GPT partition entries.\n");
        free(entries);
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = entries + (size_t)i * stride;
        if ((part && part != (int)i + 1) || !memcmp(p, (uint8_t[16]){0}, 16)) continue;
        uint64_t start = le64(p + 32), end = le64(p + 40);
        if (start < first || end < start || end > last ||
            !disk_read(disk, start * sector, header, 36)) {
            fprintf(stderr, "Cannot read partition %u.\n", i + 1);
            ok = 0;
            continue;
        }
        if (memcmp(header + 32, "NXSB", 4)) continue;
        struct slice slice = {disk, start * sector, (end - start + 1) * sector, 0};
        printf("APFS partition=%u start_lba=%llu\n", i + 1, (unsigned long long)start);
        found++;
        if (!visit(&slice, (int)i + 1, volume, path, dest)) ok = 0;
    }
    free(entries);
    if (!found) fprintf(stderr, "No matching APFS container.\n");
    return ok && found;
}

static int number(const char *s)
{
    char *end;
    errno = 0;
    if (*s < '0' || *s > '9') return -1;
    long n = strtol(s, &end, 10);
    return errno || *end || n > INT_MAX ? -1 : (int)n;
}

int main(int argc, char **argv)
{
    const char *image = NULL, *device = NULL;
    int unit = 0;
    int sector = 0, part = 0, volume = -1;
    int mounting = 0;
#ifdef __AROS__
    if (argc == 2 && !strcmp(argv[1], "--unmount")) return apfs_stop() ? 0 : 20;
#endif
    if (argc >= 4 && !strcmp(argv[1], "--image")) {
        image = argv[2];
        sector = number(argv[3]);
        argc -= 3; argv += 3;
        if (sector != 512 && sector != 4096) goto usage;
    }
#ifdef __AROS__
    else if (argc >= 4 && !strcmp(argv[1], "--device")) {
        device = argv[2];
        unit = number(argv[3]);
        if (!*device || unit < 0) goto usage;
        argc -= 3; argv += 3;
    }
#endif
    if (!image && !device) goto usage;
#ifdef __AROS__
    mounting = argc == 4 && !strcmp(argv[3], "--mount");
#endif
    if (argc != 1) {
        if ((argc != 4 && argc != 5) || (part = number(argv[1])) <= 0 ||
            (volume = number(argv[2])) < 0 || (!mounting && argv[3][0] != '/')) goto usage;
    }
    struct disk *disk = disk_open(image, sector, device, unit);
    if (!disk) { fprintf(stderr, "Cannot open input disk or determine geometry.\n"); return 20; }
    int ok = scan(disk, part, volume, argc >= 4 ? argv[3] : NULL, argc == 5 ? argv[4] : NULL);
    disk_close(disk);
    return ok ? 0 : 20;
usage:
    fprintf(stderr, "ApfsRead --image RAW SECTOR [PART VOLUME /path [new-output]]\n");
#ifdef __AROS__
    fprintf(stderr, "ApfsRead --device DEVICE UNIT [PART VOLUME /path [RAM:filename]]\n"
                    "Use PART VOLUME --mount to mount APFSTEST:, or --unmount to release it.\n");
#endif
    return 20;
}
