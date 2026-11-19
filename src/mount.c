// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
// Serve an unlocked APFS volume to AROS DOS clients.
#include "mount.h"
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/dosasl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ALIAS "APFSTEST"
#define CONTROL "ApfsRead.Control"
#define MAX_PATH_BYTES 4096
#define APFS_TYPE 0x41504653

struct handle {
    struct FileLock lock;
    struct handle *next;
    libfsapfs_file_entry_t *entry;
    uint64_t pos, size;
    uint16_t mode;
    char path[];
};

struct mount {
    libfsapfs_volume_t *volume;
    struct MsgPort *port;
    struct DosList *node;
    struct handle *list;
    struct handle *root;
};

struct stop_message {
    struct Message msg;
    int ok;
};

static void release(struct mount *m, struct handle *h)
{
    struct handle **slot = &m->list;
    while (*slot && *slot != h) slot = &(*slot)->next;
    if (*slot) {
        *slot = h->next;
        libfsapfs_file_entry_free(&h->entry, NULL);
        free(h);
    }
}

static struct handle *lookup(struct mount *m, IPTR key)
{
    struct handle *h;
    if (!key) return m->root;
    for (h = m->list; h; h = h->next)
        if (MKBADDR(&h->lock) == (BPTR)key) return h;
    return NULL;
}

static LONG open_path(struct mount *m, const char *path, struct handle **result)
{
    uint64_t id;
    libfsapfs_file_entry_t *entry = NULL;
    int rc = libfsapfs_volume_get_file_entry_by_utf8_path(m->volume, (const uint8_t *)path,
        strlen(path), &entry, NULL);
    if (rc != 1) return rc == 0 ? ERROR_OBJECT_NOT_FOUND : ERROR_NOT_A_DOS_DISK;
    struct handle *h = calloc(1, sizeof(*h) + strlen(path) + 1);
    if (!h) { libfsapfs_file_entry_free(&entry, NULL); return ERROR_NO_FREE_STORE; }
    if (libfsapfs_file_entry_get_file_mode(entry, &h->mode, NULL) != 1 ||
        libfsapfs_file_entry_get_identifier(entry, &id, NULL) != 1 ||
        ((h->mode & 0170000) == 0100000 &&
         libfsapfs_file_entry_get_size(entry, &h->size, NULL) != 1)) {
        libfsapfs_file_entry_free(&entry, NULL);
        free(h);
        return ERROR_NOT_A_DOS_DISK;
    }
    h->entry = entry;
    h->lock.fl_Access = SHARED_LOCK;
    h->lock.fl_Key = (IPTR)id;
    h->lock.fl_Task = m->port;
    h->lock.fl_Volume = MKBADDR(m->node);
    strcpy(h->path, path);
    h->next = m->list;
    m->list = h;
    *result = h;
    return 0;
}

static void parent_path(char *p)
{
    char *last = strrchr(p, '/');
    if (last) last[last == p ? 1 : 0] = 0;
}

static LONG resolve(struct mount *m, struct handle *base, BSTR bname, struct handle **result)
{
    if (!base || !bname) return ERROR_INVALID_LOCK;
    const char *name = (const char *)AROS_BSTR_ADDR(bname);
#ifdef AROS_FAST_BPTR
    size_t n = strlen(name);
#else
    size_t n = AROS_BSTR_strlen(bname);
#endif
    if (memchr(name, 0, n)) return ERROR_INVALID_COMPONENT_NAME;
    const char *colon = memchr(name, ':', n);
    const char *prefix = base->path;
    if (colon) {
        size_t k = colon - name;
        if (k && (k != sizeof(ALIAS)-1 || strncasecmp(name, ALIAS, k)))
            return ERROR_DEVICE_NOT_MOUNTED;
        prefix = "/";
        name += k + 1;
        n -= k + 1;
    } else if (n && (base->mode & 0170000) != 0040000)
        return ERROR_OBJECT_WRONG_TYPE;
    size_t cap = strlen(prefix) + n + 2;
    if (cap > MAX_PATH_BYTES) return ERROR_LINE_TOO_LONG;
    char *path = malloc(cap);
    if (!path) return ERROR_NO_FREE_STORE;
    strcpy(path, prefix);
    while (n) {
        const char *sep = memchr(name, '/', n);
        size_t k = sep ? (size_t)(sep - name) : n;
        if (!k) parent_path(path);
        else {
            size_t end = strlen(path);
            if (end > 1) path[end++] = '/';
            memcpy(path + end, name, k);
            path[end + k] = 0;
        }
        if (!sep) break;
        name += k + 1;
        n -= k + 1;
    }
    LONG err = open_path(m, path, result);
    free(path);
    return err;
}

static LONG examine(libfsapfs_file_entry_t *e, int root, IPTR next, struct FileInfoBlock *fib)
{
    uint8_t name[1024];
    uint16_t mode;
    size64_t size = 0;
    int64_t ns;
    if (root) strcpy((char *)name, ALIAS);
    else if (libfsapfs_file_entry_get_utf8_name(e, name, sizeof(name), NULL) != 1)
        return ERROR_NOT_A_DOS_DISK;
    size_t len = strlen((char *)name);
    if (len + 2 > sizeof(fib->fib_FileName) || len > 255) return ERROR_LINE_TOO_LONG;
    if (libfsapfs_file_entry_get_file_mode(e, &mode, NULL) != 1 ||
        libfsapfs_file_entry_get_modification_time(e, &ns, NULL) != 1)
        return ERROR_NOT_A_DOS_DISK;
    LONG type = ST_FILE;
    switch (mode & 0170000) {
    case 0040000: type = root ? ST_ROOT : ST_USERDIR; break;
    case 0120000: type = ST_SOFTLINK; break;
    case 0100000:
        if (libfsapfs_file_entry_get_size(e, &size, NULL) != 1) return ERROR_NOT_A_DOS_DISK;
        break;
    }
    memset(fib, 0, sizeof(*fib));
    fib->fib_DiskKey = next;
    fib->fib_EntryType = fib->fib_DirEntryType = type;
    fib->fib_FileName[0] = len;
    memcpy(fib->fib_FileName + 1, name, len);
    fib->fib_Protection = FIBF_WRITE | FIBF_DELETE;
    fib->fib_Size = size <= INT32_MAX ? (LONG)size : 0;
    uint64_t blocks = size / 512 + (size % 512 != 0);
    fib->fib_NumBlocks = blocks <= INT32_MAX ? (LONG)blocks : 0;
    int64_t sec = ns / 1000000000 - 252460800;  // Unix to Amiga epoch
    if (sec >= 0) {
        fib->fib_Date.ds_Days = sec / 86400;
        fib->fib_Date.ds_Minute = sec % 86400 / 60;
        fib->fib_Date.ds_Tick = (sec % 60) * TICKS_PER_SECOND +
            ns % 1000000000 * TICKS_PER_SECOND / 1000000000;
    }
    return 0;
}

static void dispatch(struct mount *m, struct DosPacket *p)
{
    struct handle *h = lookup(m, p->dp_Arg1), *opened = NULL;
    LONG err = 0;
    IPTR result = DOSTRUE;
    switch (p->dp_Type) {
    case ACTION_LOCATE_OBJECT:
        if (p->dp_Arg3 == EXCLUSIVE_LOCK) { err = ERROR_DISK_WRITE_PROTECTED; break; }
        err = resolve(m, h, (BSTR)p->dp_Arg2, &opened);
        if (!err) result = (IPTR)MKBADDR(&opened->lock);
        break;
    case ACTION_COPY_DIR: case ACTION_COPY_DIR_FH:
        if (!h) { err = ERROR_INVALID_LOCK; break; }
        err = open_path(m, h->path, &opened);
        if (!err) result = (IPTR)MKBADDR(&opened->lock);
        break;
    case ACTION_PARENT: case ACTION_PARENT_FH: {
        if (!h) { err = ERROR_INVALID_LOCK; break; }
        if (!strcmp(h->path, "/")) { result = 0; break; }
        char *parent = strdup(h->path);
        if (!parent) { err = ERROR_NO_FREE_STORE; break; }
        parent_path(parent);
        err = open_path(m, parent, &opened);
        free(parent);
        if (!err) result = (IPTR)MKBADDR(&opened->lock);
        break;
    }
    case ACTION_FREE_LOCK: case ACTION_END:
        if (!h || h == m->root) err = ERROR_INVALID_LOCK;
        else release(m, h);
        break;
    case ACTION_SAME_LOCK: {
        struct handle *b = lookup(m, p->dp_Arg2);
        if (!h || !b) err = ERROR_INVALID_LOCK;
        else result = h->lock.fl_Key == b->lock.fl_Key ? DOSTRUE : DOSFALSE;
        break;
    }
    case ACTION_FINDINPUT: case ACTION_FH_FROM_LOCK: {
        struct FileHandle *fh = BADDR(p->dp_Arg1);
        if (!fh) { err = ERROR_INVALID_LOCK; break; }
        if (p->dp_Type == ACTION_FINDINPUT)
            err = resolve(m, lookup(m, p->dp_Arg2), (BSTR)p->dp_Arg3, &opened);
        else {
            opened = lookup(m, p->dp_Arg2);
            if (!opened || opened == m->root) err = ERROR_INVALID_LOCK;
        }
        if (err) break;
        if ((opened->mode & 0170000) != 0100000) {
            if (p->dp_Type == ACTION_FINDINPUT) release(m, opened);
            err = ERROR_OBJECT_WRONG_TYPE;
            break;
        }
        fh->fh_Type = m->port;
        fh->fh_Port = 0;
        fh->fh_Arg1 = (IPTR)MKBADDR(&opened->lock);
        break;
    }
    case ACTION_EXAMINE_OBJECT: case ACTION_EXAMINE_FH:
        err = h ? examine(h->entry, !strcmp(h->path,"/"), 0, BADDR(p->dp_Arg2)) : ERROR_INVALID_LOCK;
        break;
    case ACTION_EXAMINE_NEXT: {
        struct FileInfoBlock *fib = BADDR(p->dp_Arg2);
        int count;
        if (!h) { err = ERROR_INVALID_LOCK; break; }
        if ((h->mode & 0170000) != 0040000) { err = ERROR_OBJECT_WRONG_TYPE; break; }
        if (libfsapfs_file_entry_get_number_of_sub_file_entries(h->entry, &count, NULL) != 1)
            { err = ERROR_NOT_A_DOS_DISK; break; }
        IPTR i = fib->fib_DiskKey;
        if (i >= (IPTR)count) { err = ERROR_NO_MORE_ENTRIES; break; }
        libfsapfs_file_entry_t *child = NULL;
        if (libfsapfs_file_entry_get_sub_file_entry_by_index(h->entry, i, &child, NULL) != 1)
            err = ERROR_NOT_A_DOS_DISK;
        else err = examine(child, 0, i + 1, fib);
        libfsapfs_file_entry_free(&child, NULL);
        break;
    }
    case ACTION_READ: {
        result = (IPTR)-1;
        if (!h || h == m->root) { err = ERROR_INVALID_LOCK; break; }
        if ((SIPTR)p->dp_Arg3 < 0) { err = ERROR_BAD_NUMBER; break; }
        size_t n = p->dp_Arg3;
        if (n > h->size - h->pos) n = h->size - h->pos;
        if (!n) { result = 0; break; }
        ssize_t got = libfsapfs_file_entry_read_buffer_at_offset(h->entry,
            (void *)p->dp_Arg2, n, h->pos, NULL);
        if (got <= 0 || (size_t)got > n) err = ERROR_NOT_A_DOS_DISK;
        else { result = got; h->pos += got; }
        break;
    }
    case ACTION_SEEK: {
        result = (IPTR)-1;
        if (!h || h == m->root) { err = ERROR_INVALID_LOCK; break; }
        int64_t off = (LONG)p->dp_Arg2;
        uint64_t base;
        switch ((LONG)p->dp_Arg3) {
        case OFFSET_BEGINNING: base = 0; break;
        case OFFSET_CURRENT: base = h->pos; break;
        case OFFSET_END: base = h->size; break;
        default: err = ERROR_SEEK_ERROR; base = 0; break;
        }
        if (err || h->pos > INT32_MAX || (off < 0 && (uint64_t)-off > base) ||
            (off >= 0 && (uint64_t)off > h->size - base)) err = ERROR_SEEK_ERROR;
        else { result = (IPTR)h->pos; h->pos = base + off; }
        break;
    }
    case ACTION_WRITE: case ACTION_SET_FILE_SIZE:
        result = (IPTR)-1;
        err = ERROR_DISK_WRITE_PROTECTED;
        break;
    case ACTION_FINDOUTPUT: case ACTION_FINDUPDATE: case ACTION_CREATE_DIR:
    case ACTION_DELETE_OBJECT: case ACTION_RENAME_OBJECT: case ACTION_SET_PROTECT:
    case ACTION_SET_COMMENT: case ACTION_SET_DATE: case ACTION_RENAME_DISK:
    case ACTION_FORMAT: case ACTION_MAKE_LINK: case ACTION_SET_OWNER:
        err = ERROR_DISK_WRITE_PROTECTED;
        break;
    case ACTION_CURRENT_VOLUME: result = (IPTR)MKBADDR(m->node); break;
    case ACTION_FLUSH: case ACTION_IS_FILESYSTEM: break;
    default: err = ERROR_ACTION_NOT_KNOWN;
    }
    if (err) {
        switch (p->dp_Type) {
        case ACTION_SEEK: case ACTION_READ: case ACTION_WRITE: case ACTION_SET_FILE_SIZE:
            result = (IPTR)-1; break;
        default: result = DOSFALSE;
        }
    }
    struct MsgPort *reply = p->dp_Port;
    p->dp_Port = m->port;
    p->dp_Res1 = (SIPTR)result;
    p->dp_Res2 = err;
    PutMsg(reply, p->dp_Link);
}

static int unpublish(struct mount *m)
{
    if (m->list != m->root || !AttemptLockDosList(LDF_ALL | LDF_WRITE)) return 0;
    RemDosEntry(m->node);
    UnLockDosList(LDF_ALL | LDF_WRITE);
    return 1;
}

int apfs_serve(libfsapfs_volume_t *volume)
{
    struct mount m = {.volume = volume};
    struct MsgPort *ctl = CreateMsgPort();
    int ok = 0;
    m.port = CreateMsgPort();
    m.node = MakeDosEntry((CONST_STRPTR)ALIAS, DLT_VOLUME);
    if (!ctl || !m.port || !m.node) goto out;
    LONG root_error = open_path(&m, "/", &m.root);
    if (root_error) { fprintf(stderr, "Cannot open APFS root (%ld).\n", (long)root_error); goto out; }
    int children;
    if (libfsapfs_file_entry_get_number_of_sub_file_entries(m.root->entry, &children, NULL) != 1)
        goto out;
    m.node->dol_Task = m.port;
    m.node->dol_misc.dol_volume.dol_DiskType = APFS_TYPE;
    struct DosList *dl = LockDosList(LDF_ALL | LDF_WRITE);
    int published = !FindDosEntry(dl, (CONST_STRPTR)ALIAS, LDF_ALL) && AddDosEntry(m.node);
    UnLockDosList(LDF_ALL | LDF_WRITE);
    if (!published) goto out;
    ctl->mp_Node.ln_Name = CONTROL;
    AddPort(ctl);
    printf("%s: mounted read-only\n", ALIAS);
    fflush(stdout);
    while (!ok) {
        ULONG signals = Wait((1UL << m.port->mp_SigBit) | (1UL << ctl->mp_SigBit) | SIGBREAKF_CTRL_C);
        struct Message *msg;
        while ((msg = GetMsg(m.port)))
            dispatch(&m, (struct DosPacket *)msg->mn_Node.ln_Name);
        while (!ok && (msg = GetMsg(ctl))) {
            struct stop_message *stop = (struct stop_message *)msg;
            if (msg->mn_Length == sizeof(*stop)) stop->ok = ok = unpublish(&m);
            ReplyMsg(msg);
        }
        if (!ok && (signals & SIGBREAKF_CTRL_C)) ok = unpublish(&m);
    }
    RemPort(ctl);
    struct Message *pending;
    while ((pending = GetMsg(ctl))) ReplyMsg(pending);
out:
    if (m.root) release(&m, m.root);
    if (m.node) FreeDosEntry(m.node);
    if (m.port) DeleteMsgPort(m.port);
    if (ctl) DeleteMsgPort(ctl);
    return ok;
}

int apfs_stop(void)
{
    struct MsgPort *reply = CreateMsgPort();
    if (!reply) return 0;
    struct stop_message stop = {0};
    stop.msg.mn_ReplyPort = reply;
    stop.msg.mn_Length = sizeof(stop);
    Forbid();
    struct MsgPort *ctl = FindPort((CONST_STRPTR)CONTROL);
    if (ctl) PutMsg(ctl, &stop.msg);
    Permit();
    if (ctl) { WaitPort(reply); GetMsg(reply); }
    DeleteMsgPort(reply);
    if (!stop.ok) fprintf(stderr, "APFSTEST: is absent or busy.\n");
    return stop.ok;
}
