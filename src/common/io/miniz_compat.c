#include "miniz_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEWLIB_PORT_AWARE
#include <io_common.h>
#include <fileXio.h>
#include <fileXio_rpc.h>
#undef NEWLIB_PORT_AWARE

#include "miniz.h"

/* All ROM-source paths reachable by the project are exposed by iomanX.
   Slurp the compressed file once and hand it to miniz's memory reader, but
   use direct fileXio calls: one large EE->IOP read avoids the repeated small
   stdio RPCs that made ZIP/GZ launch noticeably slower on cdfs/mass/SMB. */

/* AURORA_ZIP_FILEXIO_STREAM_V1_20260822
 *
 * fileXioRead(), like read(), is allowed to complete a request partially.
 * Keep the existing fast direct-IOP path, but finish the request instead of
 * treating the first short transfer as a corrupt archive.
 */
static int filexio_read_fully(int fd, void *buf, int size)
{
        unsigned char *dst = (unsigned char *)buf;
        int total = 0;

        if (fd < 0 || !buf || size < 0)
                return -1;

        while (total < size)
        {
                int n = fileXioRead(fd, dst + total, size - total);
                if (n <= 0)
                        return (total > 0) ? total : n;
                total += n;
        }

        return total;
}

/* Use miniz's user-supplied random-access reader for ZIP archives.
 *
 * The previous implementation malloc()'d a second buffer as large as the
 * complete .zip before decompression. Aurora now keeps the SNES, QuickNES
 * and PicoDrive frontends in the same 32 MiB PS2 process, so that temporary
 * whole-archive copy can fail even when the extracted ROM itself fits in
 * the permanent _RomData buffer.
 */
typedef struct MinizFileXioReader
{
        int fd;
        int size;
        int pos;
} MinizFileXioReader;

static int miniz_filexio_reader_open(
        const char *path, MinizFileXioReader *reader)
{
        int size;

        if (!reader)
                return -1;

        reader->fd = -1;
        reader->size = 0;
        reader->pos = 0;

        if (!path || !path[0])
                return -1;

        reader->fd = fileXioOpen(path, FIO_O_RDONLY, 0);
        if (reader->fd < 0)
                return -1;

        size = fileXioLseek(reader->fd, 0, FIO_SEEK_END);
        if (size <= 0)
        {
                fileXioClose(reader->fd);
                reader->fd = -1;
                return -1;
        }

        if (fileXioLseek(reader->fd, 0, FIO_SEEK_SET) < 0)
        {
                fileXioClose(reader->fd);
                reader->fd = -1;
                return -1;
        }

        reader->size = size;
        reader->pos = 0;
        return size;
}

static void miniz_filexio_reader_close(MinizFileXioReader *reader)
{
        if (!reader)
                return;

        if (reader->fd >= 0)
                fileXioClose(reader->fd);

        reader->fd = -1;
        reader->size = 0;
        reader->pos = 0;
}

static size_t miniz_filexio_read_func(
        void *opaque, mz_uint64 file_ofs, void *buf, size_t n)
{
        MinizFileXioReader *reader = (MinizFileXioReader *)opaque;
        unsigned char *dst = (unsigned char *)buf;
        size_t total = 0;
        size_t avail;

        if (!reader || reader->fd < 0 || !buf || n == 0)
                return 0;

        if (file_ofs > 0x7FFFFFFFULL ||
            file_ofs >= (mz_uint64)reader->size)
                return 0;

        avail = (size_t)((mz_uint64)reader->size - file_ofs);
        if (n > avail)
                n = avail;

        if (reader->pos != (int)file_ofs)
        {
                int pos = fileXioLseek(
                        reader->fd, (int)file_ofs, FIO_SEEK_SET);
                /* AURORA_ZIP_ISSUE36_V3_1_20261008: a successful but
                 * incomplete seek must not feed wrong bytes into miniz. */
                if (pos != (int)file_ofs)
                        return 0;
                reader->pos = pos;
        }

        while (total < n)
        {
                size_t remain = n - total;
                int want = (remain > 0x7FFFFFFFUL)
                        ? 0x7FFFFFFF : (int)remain;
                int got = fileXioRead(reader->fd, dst + total, want);

                if (got <= 0 || got > want)
                        break;

                total += (size_t)got;
                reader->pos += got;
        }

        return total;
}

static int read_file_to_alloc(const char *path, void **out_buf, int *out_size)
{
        int fd;
        int size;
        int n;
        void *buf;

        if (!path || !path[0])
                return -1;

        /* Direct fileXio calls use IOP flags, not newlib/POSIX flags.
           FIO_O_RDONLY is 1 while newlib O_RDONLY is 0. */
        fd = fileXioOpen(path, FIO_O_RDONLY, 0);
        if (fd < 0)
                return -1;

        size = fileXioLseek(fd, 0, FIO_SEEK_END);
        if (size <= 0)
        {
                fileXioClose(fd);
                return -1;
        }
        if (fileXioLseek(fd, 0, FIO_SEEK_SET) < 0)
        {
                fileXioClose(fd);
                return -1;
        }

        buf = malloc((size_t)size);
        if (!buf)
        {
                fileXioClose(fd);
                return -1;
        }

        n = filexio_read_fully(fd, buf, size);
        fileXioClose(fd);

        if (n != size)
        {
                free(buf);
                return -1;
        }

        *out_buf = buf;
        *out_size = (int)size;
        return (int)size;
}

/* Walks the variable-length gzip header (RFC 1952 section 2.3) and
   returns the byte offset where the raw DEFLATE stream starts, or -1
   if the buffer doesn't look like a valid gzip stream. */
static int parse_gzip_header(const unsigned char *data, int len)
{
        unsigned char flags;
        int off;

        if (len < 10)
                return -1;
        if (data[0] != 0x1F || data[1] != 0x8B || data[2] != 8)
                return -1;

        flags = data[3];
        off = 10;

        if (flags & 0x04) /* FEXTRA */
        {
                int xlen;
                if (off + 2 > len) return -1;
                xlen = data[off] | (data[off + 1] << 8);
                off += 2 + xlen;
                if (off > len) return -1;
        }
        if (flags & 0x08) /* FNAME */
        {
                while (off < len && data[off] != 0) off++;
                if (off >= len) return -1;
                off++;
        }
        if (flags & 0x10) /* FCOMMENT */
        {
                while (off < len && data[off] != 0) off++;
                if (off >= len) return -1;
                off++;
        }
        if (flags & 0x02) /* FHCRC */
        {
                off += 2;
                if (off > len) return -1;
        }
        return off;
}

/* AURORA_DYNAMIC_ROM_BUFFER_V1_20260823
 * GZIP metadata/prefix preflight. ISIZE is RFC1952's uncompressed size
 * modulo 2^32; supported ROMs are restricted far below that wrap point. */
int MinizGetGZUncompressedSize(const char *path)
{
        int fd, file_size, n;
        unsigned char tail[4];
        unsigned int size;

        if (!path || !path[0])
                return -1;

        fd = fileXioOpen(path, FIO_O_RDONLY, 0);
        if (fd < 0)
                return -1;

        file_size = fileXioLseek(fd, 0, FIO_SEEK_END);
        if (file_size < 18 ||
            fileXioLseek(fd, file_size - 4, FIO_SEEK_SET) < 0)
        {
                fileXioClose(fd);
                return -1;
        }

        n = filexio_read_fully(fd, tail, 4);
        fileXioClose(fd);
        if (n != 4)
                return -1;

        size = (unsigned int)tail[0] |
               ((unsigned int)tail[1] << 8) |
               ((unsigned int)tail[2] << 16) |
               ((unsigned int)tail[3] << 24);
        if (size == 0 || size > 0x7FFFFFFFU)
                return -1;
        return (int)size;
}

typedef struct MinizPrefixSink
{
        unsigned char *dst;
        int capacity;
        int written;
} MinizPrefixSink;

static int miniz_gz_prefix_sink(const void *buf, int len, void *user)
{
        MinizPrefixSink *sink = (MinizPrefixSink *)user;
        int copy;

        if (!sink || len < 0)
                return 0;

        copy = sink->capacity - sink->written;
        if (copy > len)
                copy = len;
        if (copy > 0)
        {
                memcpy(sink->dst + sink->written, buf, (size_t)copy);
                sink->written += copy;
        }

        /* Keep consuming so malformed deflate is not accepted as a probe. */
        return 1;
}

int MinizReadGZPrefix(const char *path, void *out_buf, int out_max)
{
        void *gz_data = NULL;
        int gz_size = 0, hdr, deflate_len, ok;
        size_t in_size;
        MinizPrefixSink sink;

        if (!out_buf || out_max <= 0)
                return -1;
        if (read_file_to_alloc(path, &gz_data, &gz_size) <= 0)
                return -1;

        hdr = parse_gzip_header((const unsigned char *)gz_data, gz_size);
        if (hdr < 0 || gz_size <= hdr + 8)
        {
                free(gz_data);
                return -1;
        }

        deflate_len = gz_size - hdr - 8;
        sink.dst = (unsigned char *)out_buf;
        sink.capacity = out_max;
        sink.written = 0;
        in_size = (size_t)deflate_len;

        ok = tinfl_decompress_mem_to_callback(
                (const unsigned char *)gz_data + hdr,
                &in_size, miniz_gz_prefix_sink, &sink, 0);

        free(gz_data);
        return ok ? sink.written : -1;
}


/* AURORA_LOAD_BYTE_PROGRESS_V1_20260915
 * GZ output sink: same raw-DEFLATE path as before, but copy through miniz's
 * existing callback API so the frontend can observe uncompressed bytes. */
typedef struct MinizGzBufferSink
{
        unsigned char *dst;
        int capacity;
        int written;
        MinizProgressCallback progress;
        void *progress_user;
} MinizGzBufferSink;

static int miniz_gz_buffer_sink(const void *buf, int len, void *user)
{
        MinizGzBufferSink *sink = (MinizGzBufferSink *)user;

        if (!sink || !buf || len < 0 ||
            len > sink->capacity - sink->written)
                return 0;

        if (len > 0)
        {
                memcpy(sink->dst + sink->written, buf, (size_t)len);
                sink->written += len;
                if (sink->progress)
                        sink->progress(
                            sink->written, sink->capacity,
                            sink->progress_user);
        }
        return 1;
}

int MinizReadGZToBufferProgress(const char *path,
                                void *out_buf,
                                int out_max,
                                MinizProgressCallback progress,
                                void *progress_user)
{
        void *gz_data = NULL;
        int gz_size = 0;
        int hdr;
        int deflate_len;
        int ok;
        size_t in_size;
        MinizGzBufferSink sink;

        if (!out_buf || out_max <= 0)
                return -1;

        if (read_file_to_alloc(path, &gz_data, &gz_size) <= 0)
                return -1;

        hdr = parse_gzip_header((const unsigned char *)gz_data, gz_size);
        if (hdr < 0 || gz_size <= hdr + 8)
        {
                free(gz_data);
                return -1;
        }

        deflate_len = gz_size - hdr - 8;
        sink.dst = (unsigned char *)out_buf;
        sink.capacity = out_max;
        sink.written = 0;
        sink.progress = progress;
        sink.progress_user = progress_user;
        in_size = (size_t)deflate_len;

        ok = tinfl_decompress_mem_to_callback(
                (const unsigned char *)gz_data + hdr,
                &in_size, miniz_gz_buffer_sink, &sink, 0);

        free(gz_data);
        if (!ok || sink.written <= 0)
                return -1;
        return sink.written;
}

/* AURORA_LOADING_FAST_CLEANUP_20260915
 * Static loading UI no longer needs output callbacks.  Restore miniz's
 * direct memory path: one inflate directly into the final ROM backing. */
int MinizReadGZToBuffer(const char *path, void *out_buf, int out_max)
{
        void *gz_data = NULL;
        int gz_size = 0;
        int hdr;
        int deflate_len;
        size_t produced;

        if (!out_buf || out_max <= 0)
                return -1;

        if (read_file_to_alloc(path, &gz_data, &gz_size) <= 0)
                return -1;

        hdr = parse_gzip_header((const unsigned char *)gz_data, gz_size);
        if (hdr < 0 || gz_size <= hdr + 8)
        {
                free(gz_data);
                return -1;
        }

        deflate_len = gz_size - hdr - 8;

        produced = tinfl_decompress_mem_to_mem(
                out_buf, (size_t)out_max,
                (const unsigned char *)gz_data + hdr, (size_t)deflate_len,
                0 /* raw deflate, not zlib-wrapped */);

        free(gz_data);

        if (produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
                return -1;

        return (int)produced;
}

int MinizReadZipFirstMatch(const char *path,
                           void *out_buf,
                           int out_max,
                           char *out_filename,
                           int filename_max,
                           int (*name_filter)(const char *name))
{
        MinizFileXioReader reader;
        mz_zip_archive zip;
        mz_uint num_files;
        mz_uint i;
        int result = -1;

        if (!out_buf || out_max <= 0)
                return -1;

        if (miniz_filexio_reader_open(path, &reader) <= 0)
                return -1;

        memset(&zip, 0, sizeof(zip));
        zip.m_pRead = miniz_filexio_read_func;
        zip.m_pIO_opaque = &reader;

        if (!mz_zip_reader_init(&zip, (mz_uint64)reader.size, 0))
        {
                miniz_filexio_reader_close(&reader);
                return -1;
        }

        num_files = mz_zip_reader_get_num_files(&zip);
        for (i = 0; i < num_files; i++)
        {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&zip, i, &st))
                        continue;
                if (st.m_is_directory)
                        continue;
                if (st.m_uncomp_size == 0)
                        continue;
                if (st.m_uncomp_size > (mz_uint64)out_max)
                        continue;
                if (name_filter && !name_filter(st.m_filename))
                        continue;

                if (mz_zip_reader_extract_to_mem(
                                &zip, i, out_buf,
                                (size_t)st.m_uncomp_size, 0))
                {
                        result = (int)st.m_uncomp_size;
                        if (out_filename && filename_max > 0)
                        {
                                strncpy(out_filename, st.m_filename,
                                        (size_t)(filename_max - 1));
                                out_filename[filename_max - 1] = '\0';
                        }
                        break;
                }
        }

        mz_zip_reader_end(&zip);
        miniz_filexio_reader_close(&reader);
        return result;
}
/* AURORA_DYNAMIC_ROM_BUFFER_V1_20260823
 * ZIP preflight records a central-directory index. Final extraction uses that
 * exact index, so a second scan cannot silently switch ROM members. */
static int miniz_zip_open_reader(
        const char *path, MinizFileXioReader *reader, mz_zip_archive *zip)
{
        if (miniz_filexio_reader_open(path, reader) <= 0)
                return 0;

        memset(zip, 0, sizeof(*zip));
        zip->m_pRead = miniz_filexio_read_func;
        zip->m_pIO_opaque = reader;
        if (!mz_zip_reader_init(zip, (mz_uint64)reader->size, 0))
        {
                miniz_filexio_reader_close(reader);
                return 0;
        }
        return 1;
}

static void miniz_zip_close_reader(
        MinizFileXioReader *reader, mz_zip_archive *zip)
{
        mz_zip_reader_end(zip);
        miniz_filexio_reader_close(reader);
}

int MinizProbeZipFirstMatchInfo(const char *path,
                                unsigned int *out_file_index,
                                char *out_filename,
                                int filename_max,
                                MinizZipEntryFilter entry_filter)
{
        MinizFileXioReader reader;
        mz_zip_archive zip;
        mz_uint i, num_files;
        int result = -1;

        if (!out_file_index)
                return -1;
        if (!miniz_zip_open_reader(path, &reader, &zip))
                return -1;

        num_files = mz_zip_reader_get_num_files(&zip);
        for (i = 0; i < num_files; ++i)
        {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&zip, i, &st))
                        continue;
                if (st.m_is_directory || !st.m_is_supported)
                        continue;
                if (st.m_uncomp_size == 0 ||
                    st.m_uncomp_size > 0x7FFFFFFFULL)
                        continue;
                if (entry_filter &&
                    !entry_filter(st.m_filename,
                                  (unsigned int)st.m_uncomp_size))
                        continue;

                *out_file_index = (unsigned int)i;
                result = (int)st.m_uncomp_size;
                if (out_filename && filename_max > 0)
                {
                        strncpy(out_filename, st.m_filename,
                                (size_t)(filename_max - 1));
                        out_filename[filename_max - 1] = '\0';
                }
                break;
        }

        miniz_zip_close_reader(&reader, &zip);
        return result;
}

/* AURORA_LOAD_BYTE_PROGRESS_V1_20260915
 * ZIP output sink: mz_zip_reader_extract_to_callback() already validates the
 * entry and CRC. Copy into the caller's existing ROM backing while exposing
 * only the decompressed byte count to the UI. */
typedef struct MinizZipBufferSink
{
        unsigned char *dst;
        int capacity;
        int written;
        int total;
        MinizProgressCallback progress;
        void *progress_user;
} MinizZipBufferSink;

static size_t miniz_zip_buffer_sink(void *opaque, mz_uint64 file_ofs,
                                    const void *buf, size_t n)
{
        MinizZipBufferSink *sink = (MinizZipBufferSink *)opaque;
        size_t off, end;

        if (!sink || !buf || file_ofs > 0x7FFFFFFFULL)
                return 0;

        off = (size_t)file_ofs;
        if (off > (size_t)sink->capacity ||
            n > (size_t)sink->capacity - off)
                return 0;

        memcpy(sink->dst + off, buf, n);
        end = off + n;
        if (end > (size_t)sink->written)
        {
                sink->written = (int)end;
                if (sink->progress)
                        sink->progress(
                            sink->written, sink->total,
                            sink->progress_user);
        }
        return n;
}

int MinizReadZipEntryToBufferProgress(
                              const char *path,
                              unsigned int file_index,
                              void *out_buf,
                              int out_max,
                              char *out_filename,
                              int filename_max,
                              MinizProgressCallback progress,
                              void *progress_user)
{
        MinizFileXioReader reader;
        mz_zip_archive zip;
        mz_zip_archive_file_stat st;
        MinizZipBufferSink sink;
        int result = -1;

        if (!out_buf || out_max <= 0)
                return -1;
        if (!miniz_zip_open_reader(path, &reader, &zip))
                return -1;

        if (file_index < mz_zip_reader_get_num_files(&zip) &&
            mz_zip_reader_file_stat(&zip, (mz_uint)file_index, &st) &&
            !st.m_is_directory && st.m_is_supported &&
            st.m_uncomp_size > 0 &&
            st.m_uncomp_size <= (mz_uint64)out_max &&
            st.m_uncomp_size <= 0x7FFFFFFFULL)
        {
                sink.dst = (unsigned char *)out_buf;
                sink.capacity = out_max;
                sink.written = 0;
                sink.total = (int)st.m_uncomp_size;
                sink.progress = progress;
                sink.progress_user = progress_user;

                if (mz_zip_reader_extract_to_callback(
                        &zip, (mz_uint)file_index,
                        miniz_zip_buffer_sink, &sink, 0) &&
                    sink.written == (int)st.m_uncomp_size)
                {
                        result = sink.written;
                        if (out_filename && filename_max > 0)
                        {
                                strncpy(out_filename, st.m_filename,
                                        (size_t)(filename_max - 1));
                                out_filename[filename_max - 1] = '\0';
                        }
                }
        }

        miniz_zip_close_reader(&reader, &zip);
        return result;
}

int MinizReadZipEntryToBuffer(const char *path,
                              unsigned int file_index,
                              void *out_buf,
                              int out_max,
                              char *out_filename,
                              int filename_max)
{
        MinizFileXioReader reader;
        mz_zip_archive zip;
        mz_zip_archive_file_stat st;
        int result = -1;

        if (!out_buf || out_max <= 0)
                return -1;
        if (!miniz_zip_open_reader(path, &reader, &zip))
                return -1;

        if (file_index < mz_zip_reader_get_num_files(&zip) &&
            mz_zip_reader_file_stat(&zip, (mz_uint)file_index, &st) &&
            !st.m_is_directory && st.m_is_supported &&
            st.m_uncomp_size > 0 &&
            st.m_uncomp_size <= (mz_uint64)out_max &&
            st.m_uncomp_size <= 0x7FFFFFFFULL &&
            mz_zip_reader_extract_to_mem(
                &zip, (mz_uint)file_index, out_buf,
                (size_t)st.m_uncomp_size, 0))
        {
                result = (int)st.m_uncomp_size;
                if (out_filename && filename_max > 0)
                {
                        strncpy(out_filename, st.m_filename,
                                (size_t)(filename_max - 1));
                        out_filename[filename_max - 1] = '\0';
                }
        }

        miniz_zip_close_reader(&reader, &zip);
        return result;
}

typedef struct MinizZipPrefixSink
{
        unsigned char *dst;
        int capacity;
        int written;
} MinizZipPrefixSink;

static size_t miniz_zip_prefix_sink(void *opaque, mz_uint64 file_ofs,
                                    const void *buf, size_t n)
{
        MinizZipPrefixSink *sink = (MinizZipPrefixSink *)opaque;

        if (!sink || !buf)
                return 0;

        if (file_ofs < (mz_uint64)sink->capacity)
        {
                size_t avail = (size_t)sink->capacity - (size_t)file_ofs;
                size_t copy = (n < avail) ? n : avail;
                if (copy)
                {
                        memcpy(sink->dst + (size_t)file_ofs, buf, copy);
                        if ((int)((size_t)file_ofs + copy) > sink->written)
                                sink->written =
                                    (int)((size_t)file_ofs + copy);
                }
        }
        return n;
}

int MinizReadZipEntryPrefix(const char *path,
                            unsigned int file_index,
                            void *out_buf,
                            int out_max)
{
        MinizFileXioReader reader;
        mz_zip_archive zip;
        mz_zip_archive_file_stat st;
        MinizZipPrefixSink sink;
        int result = -1;

        if (!out_buf || out_max <= 0)
                return -1;
        if (!miniz_zip_open_reader(path, &reader, &zip))
                return -1;

        sink.dst = (unsigned char *)out_buf;
        sink.capacity = out_max;
        sink.written = 0;

        if (file_index < mz_zip_reader_get_num_files(&zip) &&
            mz_zip_reader_file_stat(&zip, (mz_uint)file_index, &st) &&
            !st.m_is_directory && st.m_is_supported &&
            st.m_uncomp_size > 0 &&
            mz_zip_reader_extract_to_callback(
                &zip, (mz_uint)file_index,
                miniz_zip_prefix_sink, &sink, 0))
            result = sink.written;

        miniz_zip_close_reader(&reader, &zip);
        return result;
}
