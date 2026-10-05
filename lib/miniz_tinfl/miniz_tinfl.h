#pragma once
#include "miniz_common.h"
/* ------------------- Low-level Decompression API Definitions */

#ifndef MINIZ_NO_INFLATE_APIS

#ifdef __cplusplus
extern "C" {
#endif
/* Decompression flags used by m5tinfl_decompress(). */
/* M5TINFL_FLAG_PARSE_ZLIB_HEADER: If set, the input has a valid zlib header and ends with an adler32 checksum (it's a
 * valid zlib stream). Otherwise, the input is a raw deflate stream. */
/* M5TINFL_FLAG_HAS_MORE_INPUT: If set, there are more input bytes available beyond the end of the supplied input
 * buffer. If clear, the input buffer contains all remaining input. */
/* M5TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF: If set, the output buffer is large enough to hold the entire decompressed
 * stream. If clear, the output buffer is at least the size of the dictionary (typically 32KB). */
/* M5TINFL_FLAG_COMPUTE_ADLER32: Force adler-32 checksum computation of the decompressed bytes. */
enum {
  M5TINFL_FLAG_PARSE_ZLIB_HEADER = 1,
  M5TINFL_FLAG_HAS_MORE_INPUT = 2,
  M5TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4,
  M5TINFL_FLAG_COMPUTE_ADLER32 = 8
};

/* High level decompression functions: */
/* m5tinfl_decompress_mem_to_heap() decompresses a block in memory to a heap block allocated via malloc(). */
/* On entry: */
/*  pSrc_buf, src_buf_len: Pointer and size of the Deflate or zlib source data to decompress. */
/* On return: */
/*  Function returns a pointer to the decompressed data, or NULL on failure. */
/*  *pOut_len will be set to the decompressed data's size, which could be larger than src_buf_len on uncompressible
 * data. */
/*  The caller must call mz_free() on the returned block when it's no longer needed. */
MINIZ_EXPORT void* m5tinfl_decompress_mem_to_heap(const void* pSrc_buf, size_t src_buf_len, size_t* pOut_len,
                                                  int flags);

/* m5tinfl_decompress_mem_to_mem() decompresses a block in memory to another block in memory. */
/* Returns M5TINFL_DECOMPRESS_MEM_TO_MEM_FAILED on failure, or the number of bytes written on success. */
#define M5TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ((size_t)(-1))
MINIZ_EXPORT size_t m5tinfl_decompress_mem_to_mem(void* pOut_buf, size_t out_buf_len, const void* pSrc_buf,
                                                  size_t src_buf_len, int flags);

/* m5tinfl_decompress_mem_to_callback() decompresses a block in memory to an internal 32KB buffer, and a user provided
 * callback function will be called to flush the buffer. */
/* Returns 1 on success or 0 on failure. */
typedef int (*m5tinfl_put_buf_func_ptr)(const void* pBuf, int len, void* pUser);
MINIZ_EXPORT int m5tinfl_decompress_mem_to_callback(const void* pIn_buf, size_t* pIn_buf_size,
                                                    m5tinfl_put_buf_func_ptr pPut_buf_func, void* pPut_buf_user,
                                                    int flags);

struct m5tinfl_decompressor_tag;
typedef struct m5tinfl_decompressor_tag m5tinfl_decompressor;

#ifndef MINIZ_NO_MALLOC
/* Allocate the m5tinfl_decompressor structure in C so that */
/* non-C language bindings to m5tinfl_ API don't need to worry about */
/* structure size and allocation mechanism. */
MINIZ_EXPORT m5tinfl_decompressor* m5tinfl_decompressor_alloc(void);
MINIZ_EXPORT void m5tinfl_decompressor_free(m5tinfl_decompressor* pDecomp);
#endif

/* Max size of LZ dictionary. */
#define M5TINFL_LZ_DICT_SIZE 32768

/* Return status. */
typedef enum {
  /* This flags indicates the inflator needs 1 or more input bytes to make forward progress, but the caller is
     indicating that no more are available. The compressed data */
  /* is probably corrupted. If you call the inflator again with more bytes it'll try to continue processing the input
     but this is a BAD sign (either the data is corrupted or you called it incorrectly). */
  /* If you call it again with no input you'll just get M5TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS again. */
  M5TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS = -4,

  /* This flag indicates that one or more of the input parameters was obviously bogus. (You can try calling it again,
     but if you get this error the calling code is wrong.) */
  M5TINFL_STATUS_BAD_PARAM = -3,

  /* This flags indicate the inflator is finished but the adler32 check of the uncompressed data didn't match. If you
     call it again it'll return M5TINFL_STATUS_DONE. */
  M5TINFL_STATUS_ADLER32_MISMATCH = -2,

  /* This flags indicate the inflator has somehow failed (bad code, corrupted input, etc.). If you call it again without
     resetting via m5tinfl_init() it it'll just keep on returning the same status failure code. */
  M5TINFL_STATUS_FAILED = -1,

  /* Any status code less than M5TINFL_STATUS_DONE must indicate a failure. */

  /* This flag indicates the inflator has returned every byte of uncompressed data that it can, has consumed every byte
     that it needed, has successfully reached the end of the deflate stream, and */
  /* if zlib headers and adler32 checking enabled that it has successfully checked the uncompressed data's adler32. If
     you call it again you'll just get M5TINFL_STATUS_DONE over and over again. */
  M5TINFL_STATUS_DONE = 0,

  /* This flag indicates the inflator MUST have more input data (even 1 byte) before it can make any more forward
     progress, or you need to clear the M5TINFL_FLAG_HAS_MORE_INPUT */
  /* flag on the next call if you don't have any more source data. If the source data was somehow corrupted it's also
     possible (but unlikely) for the inflator to keep on demanding input to */
  /* proceed, so be sure to properly set the M5TINFL_FLAG_HAS_MORE_INPUT flag. */
  M5TINFL_STATUS_NEEDS_MORE_INPUT = 1,

  /* This flag indicates the inflator definitely has 1 or more bytes of uncompressed data available, but it cannot write
     this data into the output buffer. */
  /* Note if the source compressed data was corrupted it's possible for the inflator to return a lot of uncompressed
     data to the caller. I've been assuming you know how much uncompressed data to expect */
  /* (either exact or worst case) and will stop calling the inflator and fail after receiving too much. In pure
     streaming scenarios where you have no idea how many bytes to expect this may not be possible */
  /* so I may need to add some code to address this. */
  M5TINFL_STATUS_HAS_MORE_OUTPUT = 2
} m5tinfl_status;

/* Initializes the decompressor to its initial state. */
#define m5tinfl_init(r) \
  do {                  \
    (r)->m_state = 0;   \
  }                     \
  MZ_MACRO_END
#define m5tinfl_get_adler32(r) (r)->m_check_adler32

/* Main low-level decompressor coroutine function. This is the only function actually needed for decompression. All the
 * other functions are just high-level helpers for improved usability. */
/* This is a universal API, i.e. it can be used as a building block to build any desired higher level decompression API.
 * In the limit case, it can be called once per every byte input or output. */
MINIZ_EXPORT m5tinfl_status m5tinfl_decompress(m5tinfl_decompressor* r, const mz_uint8* pIn_buf_next,
                                               size_t* pIn_buf_size, mz_uint8* pOut_buf_start, mz_uint8* pOut_buf_next,
                                               size_t* pOut_buf_size, const mz_uint32 decomp_flags);

/* Internal/private bits follow. */
enum {
  M5TINFL_MAX_HUFF_TABLES = 3,
  M5TINFL_MAX_HUFF_SYMBOLS_0 = 288,
  M5TINFL_MAX_HUFF_SYMBOLS_1 = 32,
  M5TINFL_MAX_HUFF_SYMBOLS_2 = 19,
  // m5rajiruでのvendoring注記: 本家は10(1024エントリ)。m_look_up[3][1024]がm5tinfl_decompressor
  // (静的に1個だけ確保、gzip_inflate.cpp参照)の大半(6144/8364バイト)を占めるため、
  // このアプリの用途(数百バイトのプレイリストをたまに展開するだけ)には過剰と判断して
  // 8(256エントリ)に落としている。ヒット率は下がるが、外れても遅いパス(ツリー探索)へ
  // フォールバックするだけで正しさには影響しない。3*1024*2B→3*256*2Bで4608B削減。
  M5TINFL_FAST_LOOKUP_BITS = 8,
  M5TINFL_FAST_LOOKUP_SIZE = 1 << M5TINFL_FAST_LOOKUP_BITS
};

#if MINIZ_HAS_64BIT_REGISTERS
#define M5TINFL_USE_64BIT_BITBUF 1
#else
#define M5TINFL_USE_64BIT_BITBUF 0
#endif

#if M5TINFL_USE_64BIT_BITBUF
typedef mz_uint64 m5tinfl_bit_buf_t;
#define M5TINFL_BITBUF_SIZE (64)
#else
typedef mz_uint32 m5tinfl_bit_buf_t;
#define M5TINFL_BITBUF_SIZE (32)
#endif

struct m5tinfl_decompressor_tag {
  mz_uint32 m_state, m_num_bits, m_zhdr0, m_zhdr1, m_z_adler32, m_final, m_type, m_check_adler32, m_dist, m_counter,
      m_num_extra, m_table_sizes[M5TINFL_MAX_HUFF_TABLES];
  m5tinfl_bit_buf_t m_bit_buf;
  size_t m_dist_from_out_buf_start;
  mz_int16 m_look_up[M5TINFL_MAX_HUFF_TABLES][M5TINFL_FAST_LOOKUP_SIZE];
  mz_int16 m_tree_0[M5TINFL_MAX_HUFF_SYMBOLS_0 * 2];
  mz_int16 m_tree_1[M5TINFL_MAX_HUFF_SYMBOLS_1 * 2];
  mz_int16 m_tree_2[M5TINFL_MAX_HUFF_SYMBOLS_2 * 2];
  mz_uint8 m_code_size_0[M5TINFL_MAX_HUFF_SYMBOLS_0];
  mz_uint8 m_code_size_1[M5TINFL_MAX_HUFF_SYMBOLS_1];
  mz_uint8 m_code_size_2[M5TINFL_MAX_HUFF_SYMBOLS_2];
  mz_uint8 m_raw_header[4], m_len_codes[M5TINFL_MAX_HUFF_SYMBOLS_0 + M5TINFL_MAX_HUFF_SYMBOLS_1 + 137];
};

#ifdef __cplusplus
}
#endif

#endif /*#ifndef MINIZ_NO_INFLATE_APIS*/
