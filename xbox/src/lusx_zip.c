/* lusx_zip.c -- the read-only zip reader behind xbox/include/zip.h, for
 * libultraship's .o2r archives.
 *
 * An archive's central directory is read once, when its first handle opens,
 * into one shared table: 20 bytes an entry plus the names, NUL-terminated in
 * one block, and a hash index of 4 bytes a slot. Every further handle on the
 * same file (LUS keeps one per loading thread) shares it and has only its own
 * file descriptor. Entries are stored or deflated (zlib, raw inflate); ZIP64
 * directories are read.
 *
 * The disc is read through one small cache of aligned blocks, shared by every
 * archive: most entries are a few hundred bytes and a scene's sit side by side,
 * so a scene is a few dozen reads, where a read per entry cost it hundreds. */
#include "zip.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include "zlib.h"

#define BLOCK_SHIFT 14
#define BLOCK_SIZE (1u << BLOCK_SHIFT)
#define BLOCKS 16

typedef struct {
    uint32_t name_off;
    uint32_t comp_size;
    uint32_t size;
    uint32_t local_off;
    uint16_t method;
    uint16_t name_len;
} Entry;

typedef struct Dir {
    struct Dir* next;
    char* path;
    int refs;
    uint32_t count;
    Entry* entries;
    char* names;
    uint32_t* slots; /* entry index + 1, 0 empty */
    uint32_t mask;
    uint32_t file_size;
} Dir;

struct zip {
    Dir* dir;
    int fd;
    zip_error_t error;
};

struct zip_file {
    zip_t* za;
    const Entry* entry;
    uint32_t data_off;
    int done;
};

struct zip_source {
    int unused;
};

/* a block of an archive; dir is null while it holds nothing */
typedef struct {
    const Dir* dir;
    uint32_t index;
    uint32_t len;
    uint32_t used; /* the clock when last read: the lowest goes first */
    unsigned char* data;
} Block;

uint32_t gLusxZipReads;
uint64_t gLusxZipReadCycles;

static Dir* sDirs;
static mtx_t sDirsLock; /* the directories, the blocks and the inflater */
static once_flag sDirsOnce = ONCE_FLAG_INIT;
static Block sBlocks[BLOCKS];
static uint32_t sBlockClock;
static z_stream sInflater;
static int sInflaterLive;

static void dirs_init(void) {
    mtx_init(&sDirsLock, mtx_plain);
}

static uint32_t name_hash(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < n; i++) {
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    }
    return h;
}

static uint16_t rd16(const unsigned char* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void dir_free(Dir* d) {
    int i;

    for (i = 0; i < BLOCKS; i++) {
        if (sBlocks[i].dir == d) {
            sBlocks[i].dir = NULL;
            sBlocks[i].used = 0;
        }
    }
    free(d->path);
    free(d->entries);
    free(d->names);
    free(d->slots);
    free(d);
}

/* the disc: one aligned read, never past the file's end */
static int block_read(Block* b, int fd, uint32_t file_size) {
    const uint32_t start = b->index << BLOCK_SHIFT;
    const uint32_t want = file_size - start < BLOCK_SIZE ? file_size - start : BLOCK_SIZE;
    const uint64_t began = __builtin_ia32_rdtsc();
    uint32_t got = 0;

    if (lseek(fd, (off_t)start, SEEK_SET) < 0) {
        return 0;
    }
    while (got < want) {
        const ssize_t r = read(fd, b->data + got, want - got);

        if (r <= 0) {
            return 0;
        }
        got += (uint32_t)r;
    }
    b->len = got;
    gLusxZipReads++;
    gLusxZipReadCycles += __builtin_ia32_rdtsc() - began;
    return 1;
}

/* the bytes from off on in the block that holds them, and how many there are; the lock held, and good
   until the next call */
static const unsigned char* bytes_at(const Dir* d, int fd, uint32_t off, uint32_t* avail) {
    const uint32_t index = off >> BLOCK_SHIFT, in = off & (BLOCK_SIZE - 1);
    Block* b = NULL;
    Block* oldest = &sBlocks[0];
    int i;

    if (off >= d->file_size) {
        return NULL;
    }
    for (i = 0; i < BLOCKS; i++) {
        if (sBlocks[i].dir == d && sBlocks[i].index == index) {
            b = &sBlocks[i];
            break;
        }
        if (sBlocks[i].used < oldest->used) {
            oldest = &sBlocks[i];
        }
    }
    if (b == NULL) {
        b = oldest;
        if (sBlocks[0].data == NULL) {
            unsigned char* mem = (unsigned char*)malloc((size_t)BLOCK_SIZE * BLOCKS);

            if (mem == NULL) {
                return NULL;
            }
            for (i = 0; i < BLOCKS; i++) {
                sBlocks[i].data = mem + (size_t)i * BLOCK_SIZE;
            }
        }
        b->dir = NULL;
        b->used = 0;
        b->index = index;
        if (!block_read(b, fd, d->file_size)) {
            return NULL;
        }
        b->dir = d;
    }
    b->used = ++sBlockClock;
    *avail = b->len - in;
    return b->data + in;
}

static int bytes_copy(const Dir* d, int fd, uint32_t off, void* dst, uint32_t n) {
    unsigned char* out = (unsigned char*)dst;

    while (n != 0) {
        uint32_t avail;
        const unsigned char* p = bytes_at(d, fd, off, &avail);

        if (p == NULL) {
            return 0;
        }
        if (avail > n) {
            avail = n;
        }
        memcpy(out, p, avail);
        out += avail;
        off += avail;
        n -= avail;
    }
    return 1;
}

/* the end of central directory record, and ZIP64's if the classic one is full */
static int find_directory(Dir* d, int fd, uint64_t* cd_off, uint64_t* cd_size, uint64_t* count) {
    unsigned char* tail;
    off_t size;
    long n, i;
    int found = 0;

    size = lseek(fd, 0, SEEK_END);
    if (size < 22 || size > 0x7FFFFFFF) {
        return 0;
    }
    d->file_size = (uint32_t)size;
    n = size < 65557 ? (long)size : 65557;
    tail = (unsigned char*)malloc((size_t)n);
    if (tail == NULL) {
        return 0;
    }
    if (!bytes_copy(d, fd, d->file_size - (uint32_t)n, tail, (uint32_t)n)) {
        free(tail);
        return 0;
    }
    for (i = n - 22; i >= 0; i--) {
        if (rd32(tail + i) == 0x06054b50u) {
            const unsigned char* e = tail + i;

            *count = rd16(e + 10);
            *cd_size = rd32(e + 12);
            *cd_off = rd32(e + 16);
            found = 1;
            if ((*count == 0xFFFF || *cd_size == 0xFFFFFFFFu || *cd_off == 0xFFFFFFFFu) && i >= 20 &&
                rd32(e - 20) == 0x07064b50u) {
                unsigned char z[56];

                if (bytes_copy(d, fd, (uint32_t)rd64(e - 20 + 8), z, 56) && rd32(z) == 0x06064b50u) {
                    *count = rd64(z + 32);
                    *cd_size = rd64(z + 40);
                    *cd_off = rd64(z + 48);
                }
            }
            break;
        }
    }
    free(tail);
    return found;
}

static Dir* dir_read(int fd, const char* path) {
    uint64_t cd_off, cd_size, count, i;
    size_t names_cap, names_len = 0;
    uint32_t slots, at;
    Dir* d = (Dir*)calloc(1, sizeof(Dir));

    if (d == NULL) {
        return NULL;
    }
    if (!find_directory(d, fd, &cd_off, &cd_size, &count) || count > 0x7FFFFFFF || cd_off >= d->file_size) {
        dir_free(d);
        return NULL;
    }
    at = (uint32_t)cd_off;
    /* the names can be no longer than the directory less its fixed fields */
    names_cap = (size_t)(cd_size > count * 46 ? cd_size - count * 46 : 0) + (size_t)count + 1;
    for (slots = 16; slots < count * 2; slots <<= 1) {
    }
    d->path = strdup(path);
    d->entries = (Entry*)malloc(sizeof(Entry) * (size_t)(count ? count : 1));
    d->names = (char*)malloc(names_cap);
    d->slots = (uint32_t*)calloc(slots, sizeof(uint32_t));
    d->mask = slots - 1;
    if (!d->path || !d->entries || !d->names || !d->slots) {
        dir_free(d);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        unsigned char h[46];
        Entry* e = &d->entries[d->count];
        uint16_t nlen, xlen, clen;
        uint32_t s;

        if (!bytes_copy(d, fd, at, h, 46) || rd32(h) != 0x02014b50u) {
            break;
        }
        nlen = rd16(h + 28);
        xlen = rd16(h + 30);
        clen = rd16(h + 32);
        if (names_len + nlen + 1 > names_cap || !bytes_copy(d, fd, at + 46, d->names + names_len, nlen)) {
            break;
        }
        at += 46u + nlen;
        e->method = rd16(h + 10);
        e->comp_size = rd32(h + 20);
        e->size = rd32(h + 24);
        e->local_off = rd32(h + 42);
        e->name_off = (uint32_t)names_len;
        e->name_len = nlen;
        if (xlen != 0) {
            /* ZIP64: the 64-bit sizes and offset, for the fields that are full */
            unsigned char x[1024];
            unsigned off = 0;
            uint16_t take = xlen < sizeof(x) ? xlen : (uint16_t)sizeof(x);

            if (!bytes_copy(d, fd, at, x, take)) {
                break;
            }
            at += xlen;
            while (off + 4 <= take) {
                uint16_t id = rd16(x + off), len = rd16(x + off + 2);
                unsigned p = off + 4;

                if (id == 0x0001) {
                    if (e->size == 0xFFFFFFFFu && p + 8 <= off + 4 + len) {
                        e->size = (uint32_t)rd64(x + p);
                        p += 8;
                    }
                    if (e->comp_size == 0xFFFFFFFFu && p + 8 <= off + 4 + len) {
                        e->comp_size = (uint32_t)rd64(x + p);
                        p += 8;
                    }
                    if (e->local_off == 0xFFFFFFFFu && p + 8 <= off + 4 + len) {
                        e->local_off = (uint32_t)rd64(x + p);
                    }
                    break;
                }
                off += 4 + len;
            }
        }
        at += clen;
        d->names[names_len + nlen] = '\0';
        names_len += nlen + 1u;
        for (s = name_hash(d->names + e->name_off, nlen) & d->mask; d->slots[s] != 0; s = (s + 1) & d->mask) {
        }
        d->slots[s] = d->count + 1;
        d->count++;
    }
    /* give back what the names did not use */
    {
        char* shrunk = (char*)realloc(d->names, names_len ? names_len : 1);

        if (shrunk != NULL) {
            d->names = shrunk;
        }
    }
    return d;
}

zip_t* zip_open(const char* path, int flags, int* errorp) {
    int fd;
    Dir* d;
    zip_t* za;

    (void)flags;
    call_once(&sDirsOnce, dirs_init);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errorp) {
            *errorp = 11; /* ZIP_ER_OPEN */
        }
        return NULL;
    }
    mtx_lock(&sDirsLock);
    for (d = sDirs; d != NULL; d = d->next) {
        if (strcmp(d->path, path) == 0) {
            break;
        }
    }
    if (d == NULL) {
        d = dir_read(fd, path);
        if (d != NULL) {
            d->next = sDirs;
            sDirs = d;
        }
    }
    if (d != NULL) {
        d->refs++;
    }
    mtx_unlock(&sDirsLock);
    za = d != NULL ? (zip_t*)calloc(1, sizeof(zip_t)) : NULL;
    if (za == NULL) {
        close(fd);
        if (errorp) {
            *errorp = 19; /* ZIP_ER_NOZIP */
        }
        return NULL;
    }
    za->dir = d;
    za->fd = fd;
    return za;
}

static void release(zip_t* za) {
    Dir** pp;

    mtx_lock(&sDirsLock);
    if (--za->dir->refs == 0) {
        for (pp = &sDirs; *pp != NULL; pp = &(*pp)->next) {
            if (*pp == za->dir) {
                *pp = za->dir->next;
                break;
            }
        }
        dir_free(za->dir);
    }
    mtx_unlock(&sDirsLock);
    close(za->fd);
    free(za);
}

int zip_close(zip_t* za) {
    if (za != NULL) {
        release(za);
    }
    return 0;
}

void zip_discard(zip_t* za) {
    zip_close(za);
}

zip_int64_t zip_get_num_entries(zip_t* za, zip_flags_t flags) {
    (void)flags;
    return za ? (zip_int64_t)za->dir->count : -1;
}

const char* zip_get_name(zip_t* za, zip_uint64_t index, zip_flags_t flags) {
    (void)flags;
    if (za == NULL || index >= za->dir->count) {
        return NULL;
    }
    return za->dir->names + za->dir->entries[index].name_off;
}

zip_int64_t zip_name_locate(zip_t* za, const char* fname, zip_flags_t flags) {
    const Dir* d;
    size_t n;
    uint32_t s;

    (void)flags;
    if (za == NULL || fname == NULL) {
        return -1;
    }
    d = za->dir;
    n = strlen(fname);
    for (s = name_hash(fname, n) & d->mask; d->slots[s] != 0; s = (s + 1) & d->mask) {
        const Entry* e = &d->entries[d->slots[s] - 1];

        if (e->name_len == n && memcmp(d->names + e->name_off, fname, n) == 0) {
            return (zip_int64_t)(d->slots[s] - 1);
        }
    }
    return -1;
}

void zip_stat_init(struct zip_stat* st) {
    memset(st, 0, sizeof(*st));
}

int zip_stat_index(zip_t* za, zip_uint64_t index, zip_flags_t flags, struct zip_stat* st) {
    const Entry* e;

    (void)flags;
    if (za == NULL || index >= za->dir->count) {
        return -1;
    }
    e = &za->dir->entries[index];
    zip_stat_init(st);
    st->valid = ZIP_STAT_NAME | ZIP_STAT_INDEX | ZIP_STAT_SIZE | ZIP_STAT_COMP_SIZE | ZIP_STAT_COMP_METHOD;
    st->name = za->dir->names + e->name_off;
    st->index = index;
    st->size = e->size;
    st->comp_size = e->comp_size;
    st->comp_method = e->method;
    return 0;
}

zip_file_t* zip_fopen_index(zip_t* za, zip_uint64_t index, zip_flags_t flags) {
    const Entry* e;
    unsigned char lh[30];
    zip_file_t* zf;
    int read_ok;

    (void)flags;
    if (za == NULL || index >= za->dir->count) {
        return NULL;
    }
    e = &za->dir->entries[index];
    if (e->method != 0 && e->method != 8) {
        return NULL;
    }
    mtx_lock(&sDirsLock);
    read_ok = bytes_copy(za->dir, za->fd, e->local_off, lh, 30);
    mtx_unlock(&sDirsLock);
    if (!read_ok || rd32(lh) != 0x04034b50u) {
        return NULL;
    }
    zf = (zip_file_t*)malloc(sizeof(zip_file_t));
    if (zf == NULL) {
        return NULL;
    }
    zf->za = za;
    zf->entry = e;
    zf->data_off = e->local_off + 30u + rd16(lh + 26) + rd16(lh + 28);
    zf->done = 0;
    return zf;
}

/* a deflated entry, in one go: straight from the blocks to its buffer */
static int inflate_entry(const zip_file_t* zf, void* buf) {
    const Entry* e = zf->entry;
    uint32_t off = zf->data_off, left = e->comp_size;

    if (!sInflaterLive) {
        if (inflateInit2(&sInflater, -MAX_WBITS) != Z_OK) {
            return 0;
        }
        sInflaterLive = 1;
    } else if (inflateReset(&sInflater) != Z_OK) {
        return 0;
    }
    sInflater.next_in = NULL;
    sInflater.avail_in = 0;
    sInflater.next_out = (Bytef*)buf;
    sInflater.avail_out = e->size;
    while (left != 0) {
        uint32_t avail;
        const unsigned char* p = bytes_at(zf->za->dir, zf->za->fd, off, &avail);
        int r;

        if (p == NULL) {
            return 0;
        }
        if (avail > left) {
            avail = left;
        }
        off += avail;
        left -= avail;
        sInflater.next_in = (Bytef*)p;
        sInflater.avail_in = avail;
        r = inflate(&sInflater, Z_FINISH);
        if (r == Z_STREAM_END) {
            return sInflater.avail_out == 0;
        }
        /* short of input is all that may stop it here: the next block has the rest */
        if (r != Z_BUF_ERROR || sInflater.avail_in != 0) {
            return 0;
        }
    }
    return 0;
}

/* an entry is read whole, in one call (all O2rArchive does): the blocks and the inflater are shared, and
   this keeps them to one call's lock */
zip_int64_t zip_fread(zip_file_t* zf, void* buf, zip_uint64_t nbytes) {
    const Entry* e;
    int read_ok;

    if (zf == NULL) {
        return -1;
    }
    e = zf->entry;
    if (zf->done || e->size == 0) {
        return 0;
    }
    if (nbytes < e->size) {
        return -1;
    }
    mtx_lock(&sDirsLock);
    read_ok = e->method == 0 ? bytes_copy(zf->za->dir, zf->za->fd, zf->data_off, buf, e->size)
                             : inflate_entry(zf, buf);
    mtx_unlock(&sDirsLock);
    zf->done = 1;
    return read_ok ? (zip_int64_t)e->size : -1;
}

int zip_fclose(zip_file_t* zf) {
    if (zf == NULL) {
        return -1;
    }
    free(zf);
    return 0;
}

static zip_source_t sSource;
static zip_error_t sReadOnly = {ZIP_ER_RDONLY, 0};

zip_source_t* zip_source_buffer(zip_t* za, const void* data, zip_uint64_t len, int freep) {
    (void)za;
    (void)data;
    (void)len;
    (void)freep;
    return &sSource;
}

void zip_source_free(zip_source_t* zs) {
    (void)zs;
}

zip_int64_t zip_file_add(zip_t* za, const char* name, zip_source_t* source, zip_flags_t flags) {
    (void)name;
    (void)source;
    (void)flags;
    if (za != NULL) {
        za->error = sReadOnly;
    }
    return -1;
}

zip_error_t* zip_get_error(zip_t* za) {
    return za != NULL ? &za->error : &sReadOnly;
}

const char* zip_error_strerror(zip_error_t* ze) {
    return ze != NULL && ze->zip_err == ZIP_ER_RDONLY ? "read-only archive (Lus-X)" : "zip error";
}

int zip_error_code_zip(const zip_error_t* ze) {
    return ze != NULL ? ze->zip_err : 0;
}
