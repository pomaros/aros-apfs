// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
#ifndef APFS_PROBE_MOUNT_H
#define APFS_PROBE_MOUNT_H
#include <libfsapfs.h>
int apfs_serve(libfsapfs_volume_t *volume);
int apfs_stop(void);
#endif
