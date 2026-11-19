// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
#ifndef APFS_READ_DISK_H
#define APFS_READ_DISK_H

#include <stddef.h>
#include <stdint.h>

struct disk;
struct disk *disk_open(const char *image, unsigned sector, const char *device, unsigned unit);
void disk_close(struct disk *disk);
unsigned disk_sector(const struct disk *disk);
uint64_t disk_length(const struct disk *disk);
int disk_read(struct disk *disk, uint64_t offset, void *out, size_t length);

#endif
