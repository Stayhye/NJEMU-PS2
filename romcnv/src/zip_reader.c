#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <miniz.h>

#include "zip_reader.h"

typedef struct zip_reader_archive_state_t
{
    mz_zip_archive archive;
} zip_reader_archive_state_t;

typedef struct zip_reader_entry_state_t
{
    mz_zip_reader_extract_iter_state *reader;
    uint64_t size;
    uint64_t bytes_read;
    uint32_t crc32;
    unsigned char byte_cache[4096];
    size_t byte_cache_pos;
    size_t byte_cache_len;
} zip_reader_entry_state_t;

static bool zip_reader_info_from_stat(const mz_zip_archive_file_stat *stat,
                                      zip_reader_entry_info_t *info)
{
    if (stat == NULL || info == NULL)
        return false;

    strncpy(info->name, stat->m_filename, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->size = stat->m_uncomp_size;
    info->crc32 = stat->m_crc32;
    return true;
}

bool zip_reader_archive_open(zip_reader_archive_t *archive, const char *path)
{
    zip_reader_archive_state_t *state;

    if (archive == NULL || path == NULL || archive->state != NULL)
        return false;

    state = calloc(1, sizeof(*state));
    if (state == NULL)
        return false;

    if (!mz_zip_reader_init_file(&state->archive, path, 0))
    {
        free(state);
        return false;
    }

    archive->state = state;
    return true;
}

void zip_reader_archive_close(zip_reader_archive_t *archive)
{
    zip_reader_archive_state_t *state;

    if (archive == NULL)
        return;

    state = archive->state;
    if (state != NULL)
    {
        mz_zip_reader_end(&state->archive);
        free(state);
    }

    memset(archive, 0, sizeof(*archive));
}

bool zip_reader_archive_stat(zip_reader_archive_t *archive,
                             const char *name,
                             zip_reader_entry_info_t *info)
{
    zip_reader_archive_state_t *state;
    mz_zip_archive_file_stat stat;
    int index;

    if (archive == NULL || name == NULL || info == NULL)
        return false;

    state = archive->state;
    if (state == NULL)
        return false;

    index = mz_zip_reader_locate_file(&state->archive, name, NULL, 0);
    if (index < 0)
        return false;
    if (!mz_zip_reader_file_stat(&state->archive, (mz_uint)index, &stat))
        return false;

    return zip_reader_info_from_stat(&stat, info);
}

bool zip_reader_archive_find_crc(zip_reader_archive_t *archive,
                                 uint32_t crc32,
                                 zip_reader_entry_info_t *info)
{
    zip_reader_archive_state_t *state;
    mz_uint i;
    mz_uint count;

    if (archive == NULL || info == NULL)
        return false;

    state = archive->state;
    if (state == NULL)
        return false;

    count = mz_zip_reader_get_num_files(&state->archive);
    for (i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat stat;

        if (!mz_zip_reader_file_stat(&state->archive, i, &stat))
            continue;
        if (stat.m_crc32 == crc32)
            return zip_reader_info_from_stat(&stat, info);
    }

    return false;
}

bool zip_reader_entry_open(zip_reader_archive_t *archive,
                           const char *name,
                           zip_reader_entry_t *entry)
{
    zip_reader_archive_state_t *archive_state;
    zip_reader_entry_state_t *entry_state;
    mz_zip_archive_file_stat stat;
    int index;

    if (archive == NULL || name == NULL || entry == NULL || entry->state != NULL)
        return false;

    archive_state = archive->state;
    if (archive_state == NULL)
        return false;

    entry_state = calloc(1, sizeof(*entry_state));
    if (entry_state == NULL)
        return false;

    index = mz_zip_reader_locate_file(&archive_state->archive, name, NULL, 0);
    if (index < 0)
        goto error;
    if (!mz_zip_reader_file_stat(&archive_state->archive, (mz_uint)index, &stat))
        goto error;

    entry_state->reader = mz_zip_reader_extract_iter_new(&archive_state->archive,
                                                        (mz_uint)index,
                                                        0);
    if (entry_state->reader == NULL)
        goto error;

    entry_state->size = stat.m_uncomp_size;
    entry_state->crc32 = stat.m_crc32;
    entry->state = entry_state;
    return true;

error:
    free(entry_state);
    return false;
}

size_t zip_reader_entry_read(zip_reader_entry_t *entry, void *dst, size_t size)
{
    zip_reader_entry_state_t *state;
    size_t result;

    if (entry == NULL || dst == NULL || size == 0)
        return 0;

    state = entry->state;
    if (state == NULL || state->reader == NULL)
        return 0;

    result = mz_zip_reader_extract_iter_read(state->reader, dst, size);
    state->bytes_read += result;
    return result;
}

int zip_reader_entry_getc(zip_reader_entry_t *entry)
{
    zip_reader_entry_state_t *state;

    if (entry == NULL)
        return EOF;

    state = entry->state;
    if (state == NULL || state->reader == NULL)
        return EOF;

    if (state->byte_cache_pos >= state->byte_cache_len)
    {
        state->byte_cache_len = zip_reader_entry_read(entry,
                                                      state->byte_cache,
                                                      sizeof(state->byte_cache));
        state->byte_cache_pos = 0;
        if (state->byte_cache_len == 0)
            return EOF;
    }

    return state->byte_cache[state->byte_cache_pos++] & 0xff;
}

bool zip_reader_entry_is_open(const zip_reader_entry_t *entry)
{
    return entry != NULL && entry->state != NULL;
}

bool zip_reader_entry_close(zip_reader_entry_t *entry)
{
    zip_reader_entry_state_t *state;
    bool complete;
    bool ok = true;

    if (entry == NULL)
        return false;

    state = entry->state;
    if (state != NULL)
    {
        complete = state->bytes_read >= state->size;
        if (state->reader != NULL)
        {
            mz_bool reader_ok = mz_zip_reader_extract_iter_free(state->reader);
            ok = !complete || reader_ok != 0;
        }
        free(state);
    }

    memset(entry, 0, sizeof(*entry));
    return ok;
}
