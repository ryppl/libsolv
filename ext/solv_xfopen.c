/*
 * Copyright (c) 2011, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <fcntl.h>

#include "solv_xfopen.h"


static FILE *cookieopen(void *cookie, const char *mode,
	ssize_t (*cread)(void *, char *, size_t), 
	ssize_t (*cwrite)(void *, const char *, size_t), 
	int (*cclose)(void *))
{
  if (!cookie)
    return 0;
#ifdef HAVE_FUNOPEN
  return funopen(cookie, 
      (int (*)(void *, char *, int))(*mode == 'r' ? cread: NULL),/* readfn */
      (int (*)(void *, const char *, int))(*mode == 'w' ? cwrite : NULL), /* writefn */
      (fpos_t (*)(void *, fpos_t, int))NULL, /* seekfn */
      cclose
      );
#elif defined(HAVE_FOPENCOOKIE)
  cookie_io_functions_t cio;
  memset(&cio, 0, sizeof(cio));
  if (*mode == 'r')
    cio.read = cread;
  else if (*mode == 'w')
    cio.write = cwrite;
  cio.close = cclose;
  return  fopencookie(cookie, *mode == 'w' ? "w" : "r", cio);
#else
# error Need to implement custom I/O
#endif
}


/* gzip compression */

static ssize_t cookie_gzread(void *cookie, char *buf, size_t nbytes)
{
  return gzread((gzFile *)cookie, buf, nbytes);
}

static ssize_t cookie_gzwrite(void *cookie, const char *buf, size_t nbytes)
{
  return gzwrite((gzFile *)cookie, buf, nbytes);
}

static int cookie_gzclose(void *cookie)
{
  return gzclose((gzFile *)cookie);
}

static inline FILE *mygzfopen(const char *fn, const char *mode)
{
  gzFile *gzf = gzopen(fn, mode);
  return cookieopen(gzf, mode, cookie_gzread, cookie_gzwrite, cookie_gzclose);
}

static inline FILE *mygzfdopen(int fd, const char *mode)
{
  gzFile *gzf = gzdopen(fd, mode);
  return cookieopen(gzf, mode, cookie_gzread, cookie_gzwrite, cookie_gzclose);
}


#ifdef ENABLE_LZMA_COMPRESSION

#include <lzma.h>

typedef struct lzfile {
  unsigned char buf[1 << 15];
  lzma_stream strm;
  FILE *file;
  int encoding;
  int eof;
} LZFILE;

static LZFILE *lzopen(const char *path, const char *mode, int fd, int xz)
{
  int level = 7;
  int encoding = 0;
  FILE *fp;
  LZFILE *lzfile;
  lzma_ret ret;
  lzma_stream init_strm = LZMA_STREAM_INIT;

  if (!path && fd < 0)
    return 0;
  for (; *mode; mode++)
    {
      if (*mode == 'w')
	encoding = 1;
      else if (*mode == 'r')
	encoding = 0;
      else if (*mode >= '1' && *mode <= '9')
	level = *mode - '0';
    }
  if (fd != -1)
    fp = fdopen(fd, encoding ? "w" : "r");
  else
    fp = fopen(path, encoding ? "w" : "r");
  if (!fp)
    return 0;
  lzfile = calloc(1, sizeof(*lzfile));
  if (!lzfile)
    {
      fclose(fp);
      return 0;
    }
  lzfile->file = fp;
  lzfile->encoding = encoding;
  lzfile->eof = 0;
  lzfile->strm = init_strm;
  if (encoding)
    {
      if (xz)
	ret = lzma_easy_encoder(&lzfile->strm, level, LZMA_CHECK_SHA256);
      else
	{
	  lzma_options_lzma options;
	  lzma_lzma_preset(&options, level);
	  ret = lzma_alone_encoder(&lzfile->strm, &options);
	}
    }
  else
    {
      /* lzma_easy_decoder_memusage(level) is not ready yet, use hardcoded limit for now */
      ret = lzma_auto_decoder(&lzfile->strm, 100 << 20, 0);
    }
  if (ret != LZMA_OK)
    {
      fclose(fp);
      free(lzfile);
      return 0;
    }
  return lzfile;
}

static int lzclose(void *cookie)
{
  LZFILE *lzfile = cookie;
  lzma_ret ret;
  size_t n;
  int rc;

  if (!lzfile)
    return -1;
  if (lzfile->encoding)
    {
      for (;;)
	{
	  lzfile->strm.avail_out = sizeof(lzfile->buf);
	  lzfile->strm.next_out = lzfile->buf;
	  ret = lzma_code(&lzfile->strm, LZMA_FINISH);
	  if (ret != LZMA_OK && ret != LZMA_STREAM_END)
	    return -1;
	  n = sizeof(lzfile->buf) - lzfile->strm.avail_out;
	  if (n && fwrite(lzfile->buf, 1, n, lzfile->file) != n)
	    return -1;
	  if (ret == LZMA_STREAM_END)
	    break;
	}
    }
  lzma_end(&lzfile->strm);
  rc = fclose(lzfile->file);
  free(lzfile);
  return rc;
}

static ssize_t lzread(void *cookie, char *buf, size_t len)
{
  LZFILE *lzfile = cookie;
  lzma_ret ret;
  int eof = 0;

  if (!lzfile || lzfile->encoding)
    return -1;
  if (lzfile->eof)
    return 0;
  lzfile->strm.next_out = (unsigned char *)buf;
  lzfile->strm.avail_out = len;
  for (;;)
    {
      if (!lzfile->strm.avail_in)
	{
	  lzfile->strm.next_in = lzfile->buf;
	  lzfile->strm.avail_in = fread(lzfile->buf, 1, sizeof(lzfile->buf), lzfile->file);
	  if (!lzfile->strm.avail_in)
	    eof = 1;
	}
      ret = lzma_code(&lzfile->strm, LZMA_RUN);
      if (ret == LZMA_STREAM_END)
	{
	  lzfile->eof = 1;
	  return len - lzfile->strm.avail_out;
	}
      if (ret != LZMA_OK)
	return -1;
      if (!lzfile->strm.avail_out)
	return len;
      if (eof)
	return -1;
    }
}

static ssize_t lzwrite(void *cookie, const char *buf, size_t len)
{
  LZFILE *lzfile = cookie;
  lzma_ret ret;
  size_t n;
  if (!lzfile || !lzfile->encoding)
    return -1;
  if (!len)
    return 0;
  lzfile->strm.next_in = (unsigned char *)buf;
  lzfile->strm.avail_in = len;
  for (;;)
    {
      lzfile->strm.next_out = lzfile->buf;
      lzfile->strm.avail_out = sizeof(lzfile->buf);
      ret = lzma_code(&lzfile->strm, LZMA_RUN);
      if (ret != LZMA_OK)
	return -1;
      n = sizeof(lzfile->buf) - lzfile->strm.avail_out;
      if (n && fwrite(lzfile->buf, 1, n, lzfile->file) != n)
	return -1;
      if (!lzfile->strm.avail_in)
	return len;
    }
}

static inline FILE *myxzfopen(const char *fn, const char *mode)
{
  LZFILE *lzf = lzopen(fn, mode, -1, 1);
  return cookieopen(lzf, mode, lzread, lzwrite, lzclose);
}

static inline FILE *myxzfdopen(int fd, const char *mode)
{
  LZFILE *lzf = lzopen(0, mode, fd, 1);
  return cookieopen(lzf, mode, lzread, lzwrite, lzclose);
}

static inline FILE *mylzfopen(const char *fn, const char *mode)
{
  LZFILE *lzf = lzopen(fn, mode, -1, 0);
  return cookieopen(lzf, mode, lzread, lzwrite, lzclose);
}

static inline FILE *mylzfdopen(int fd, const char *mode)
{
  LZFILE *lzf = lzopen(0, mode, fd, 0);
  return cookieopen(lzf, mode, lzread, lzwrite, lzclose);
}

#endif /* ENABLE_LZMA_COMPRESSION */


FILE *
solv_xfopen(const char *fn, const char *mode)
{
  char *suf;

  if (!fn)
    return 0;
  if (!mode)
    mode = "r";
  suf = strrchr(fn, '.');
  if (suf && !strcmp(suf, ".gz"))
    return mygzfopen(fn, mode);
#ifdef ENABLE_LZMA_COMPRESSION
  if (suf && !strcmp(suf, ".xz"))
    return myxzfopen(fn, mode);
  if (suf && !strcmp(suf, ".lzma"))
    return mylzfopen(fn, mode);
#endif
  return fopen(fn, mode);
}

FILE *
solv_xfopen_fd(const char *fn, int fd, const char *mode)
{
  char *suf;

  suf = fn ? strrchr(fn, '.') : 0;
  if (!mode)
    {
      int fl = fcntl(fd, F_GETFL, 0);
      if (fl == -1)
	return 0;
      fl &= O_RDONLY|O_WRONLY|O_RDWR;
      if (fl == O_WRONLY)
	mode = "w";
      else if (fl == O_RDWR)
	{
	  if (!suf || strcmp(suf, ".gz") != 0)
	    mode = "r+";
	  else
	    mode = "r";
	}
      else
	mode = "r";
    }
  if (suf && !strcmp(suf, ".gz"))
    return mygzfdopen(fd, mode);
#ifdef ENABLE_LZMA_COMPRESSION
  if (suf && !strcmp(suf, ".xz"))
    return myxzfdopen(fd, mode);
  if (suf && !strcmp(suf, ".lzma"))
    return mylzfdopen(fd, mode);
#endif
  return fdopen(fd, mode);
}

