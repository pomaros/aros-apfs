// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
#include "disk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef __AROS__
#include <exec/types.h>
#include <devices/newstyle.h>
#include <devices/trackdisk.h>
#include <proto/exec.h>
#endif

struct disk {
    FILE *image;
    uint64_t length;
    unsigned sector;
#ifdef __AROS__
    struct MsgPort *reply;
    struct IOExtTD *request;
    int opened;
    unsigned char buffer[4096];
    uint64_t buffered_at;
#endif
};

#ifdef __AROS__
static int open_device(struct disk *d, const char *name, unsigned unit)
{
    struct DriveGeometry geometry = {0};
    d->reply = CreateMsgPort();
    if (!d->reply) return 0;
    d->request = (struct IOExtTD *)CreateIORequest(d->reply, sizeof(*d->request));
    if (!d->request || OpenDevice((CONST_STRPTR)name, unit,
                (struct IORequest *)d->request, 0) != 0) return 0;
    d->opened = 1;
    struct IOStdReq *io = &d->request->iotd_Req;
    io->io_Command = TD_GETGEOMETRY;
    io->io_Data = &geometry;
    io->io_Length = sizeof(geometry);
    if (DoIO((struct IORequest *)io) != 0) return 0;
    d->sector = geometry.dg_SectorSize;
    d->length = (uint64_t)d->sector * geometry.dg_TotalSectors;
    d->buffered_at = UINT64_MAX;
    return 1;
}
#endif

struct disk *disk_open(const char *image, unsigned sector, const char *device, unsigned unit)
{
    struct disk *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (image) {
        d->image = fopen(image, "rb");
        if (!d->image || fseeko(d->image, 0, SEEK_END) != 0) goto fail;
        off_t end = ftello(d->image);
        if (end < 0) goto fail;
        d->length = (uint64_t)end;
        d->sector = sector;
    } else {
#ifdef __AROS__
        if (!device || !open_device(d, device, unit)) goto fail;
#else
        (void)device;
        (void)unit;
        goto fail;
#endif
    }
    if ((d->sector == 512 || d->sector == 4096) && d->length >= 2u * d->sector)
        return d;
fail:
    disk_close(d);
    return NULL;
}

void disk_close(struct disk *d)
{
    if (!d) return;
#ifdef __AROS__
    if (d->opened) CloseDevice((struct IORequest *)d->request);
    if (d->request) DeleteIORequest((struct IORequest *)d->request);
    if (d->reply) DeleteMsgPort(d->reply);
#endif
    if (d->image) fclose(d->image);
    free(d);
}

unsigned disk_sector(const struct disk *d) { return d->sector; }
uint64_t disk_length(const struct disk *d) { return d->length; }

int disk_read(struct disk *d, uint64_t offset, void *out, size_t length)
{
    if (offset > d->length || length > d->length - offset) return 0;
    if (d->image)
        return fseeko(d->image, (off_t)offset, SEEK_SET) == 0 &&
            fread(out, 1, length, d->image) == length;
#ifdef __AROS__
    unsigned char *bytes = out;
    while (length) {
        size_t skip = offset % d->sector;
        uint64_t aligned = offset - skip;
        if (d->buffered_at != aligned) {
            struct IOStdReq *io = &d->request->iotd_Req;
            io->io_Command = NSCMD_TD_READ64;
            io->io_Offset = (uint32_t)aligned;
            io->io_Actual = (uint32_t)(aligned >> 32);
            io->io_Data = d->buffer;
            io->io_Length = d->sector;
            d->buffered_at = UINT64_MAX;
            if (DoIO((struct IORequest *)io) || io->io_Actual != d->sector) return 0;
            d->buffered_at = aligned;
        }
        size_t n = d->sector - skip;
        if (n > length) n = length;
        memcpy(bytes, d->buffer + skip, n);
        bytes += n;
        length -= n;
        offset += n;
    }
    return 1;
#else
    return 0;
#endif
}
