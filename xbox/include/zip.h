/* zip.h -- Lus-X: the part of libzip's API libultraship's O2rArchive uses,
 * read-only (xbox/src/lusx_zip.c). An archive's central directory is read
 * once, into one compact table every handle on that file shares: LUS opens a
 * handle per loading thread, and libzip's would each hold their own copy of
 * mm.o2r's tens of thousands of entries. Writing (O2rArchive::WriteFile) is
 * refused: the disc is read-only. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t zip_int64_t;
typedef uint64_t zip_uint64_t;
typedef uint32_t zip_flags_t;

typedef struct zip zip_t;
typedef struct zip_file zip_file_t;
typedef struct zip_source zip_source_t;
typedef struct zip_error {
    int zip_err;
    int sys_err;
} zip_error_t;

#define ZIP_CREATE 1
#define ZIP_RDONLY 16
#define ZIP_FL_ENC_UTF_8 2048u
#define ZIP_FL_OVERWRITE 8192u
#define ZIP_ER_RDONLY 25

#define ZIP_STAT_NAME 0x0001u
#define ZIP_STAT_INDEX 0x0002u
#define ZIP_STAT_SIZE 0x0004u
#define ZIP_STAT_COMP_SIZE 0x0008u
#define ZIP_STAT_COMP_METHOD 0x0040u

struct zip_stat {
    zip_uint64_t valid;
    const char* name;
    zip_uint64_t index;
    zip_uint64_t size;
    zip_uint64_t comp_size;
    int64_t mtime;
    uint32_t crc;
    uint16_t comp_method;
    uint16_t encryption_method;
    uint32_t flags;
};

zip_t* zip_open(const char* path, int flags, int* errorp);
int zip_close(zip_t* za);
void zip_discard(zip_t* za);
zip_int64_t zip_get_num_entries(zip_t* za, zip_flags_t flags);
const char* zip_get_name(zip_t* za, zip_uint64_t index, zip_flags_t flags);
zip_int64_t zip_name_locate(zip_t* za, const char* fname, zip_flags_t flags);
void zip_stat_init(struct zip_stat* st);
int zip_stat_index(zip_t* za, zip_uint64_t index, zip_flags_t flags, struct zip_stat* st);
zip_file_t* zip_fopen_index(zip_t* za, zip_uint64_t index, zip_flags_t flags);
zip_int64_t zip_fread(zip_file_t* zf, void* buf, zip_uint64_t nbytes);
int zip_fclose(zip_file_t* zf);

/* Lus-X: the disc reads so far, and the CPU cycles spent in them */
extern uint32_t gLusxZipReads;
extern uint64_t gLusxZipReadCycles;

/* writing: refused */
zip_source_t* zip_source_buffer(zip_t* za, const void* data, zip_uint64_t len, int freep);
void zip_source_free(zip_source_t* zs);
zip_int64_t zip_file_add(zip_t* za, const char* name, zip_source_t* source, zip_flags_t flags);
zip_error_t* zip_get_error(zip_t* za);
const char* zip_error_strerror(zip_error_t* ze);
int zip_error_code_zip(const zip_error_t* ze);

#ifdef __cplusplus
}
#endif
