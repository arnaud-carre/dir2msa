/*
 * zipio.c - stdio emulation library for reading zip files
 *
 * Version 1.2
 */

/*
 * Copyright (c) 1995, Edward B. Hamrick
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that
 *
 * (i)  the above copyright notice and the text in this "C" comment block
 *      appear in all copies of the software and related documentation, and
 *
 * (ii) any modifications to this source file must be sent, via e-mail
 *      to the copyright owner (currently hamrick@primenet.com) within
 *      30 days of such modification.
 *
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 *
 * IN NO EVENT SHALL EDWARD B. HAMRICK BE LIABLE FOR ANY SPECIAL, INCIDENTAL,
 * INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND, OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER OR NOT ADVISED OF
 * THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF LIABILITY, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


/*
 * Refer to zipio.h for a description of this package.
 */

/*
 * The .zip file header is described below.  It consists of
 * 30 fixed bytes, followed by two variable length fields
 * whose length is contained in the first 30 bytes.  After this
 * header, the data is stored (in deflate format if the compression
 * method is 8).
 *
 * The crc-32 field is the crc on the uncompressed data.
 *
 * .zip file header:
 *
 *      local file header signature     4 bytes  (0x04034b50)
 *      version needed to extract       2 bytes
 *      general purpose bit flag        2 bytes
 *      compression method              2 bytes
 *      last mod file time              2 bytes
 *      last mod file date              2 bytes
 *      crc-32                          4 bytes
 *      compressed size                 4 bytes
 *      uncompressed size               4 bytes
 *      filename length                 2 bytes
 *      extra field length              2 bytes
 *
 *      filename (variable size)
 *      extra field (variable size)
 *
 * These fields are described in more detail in appnote.txt
 * in the pkzip 1.93 distribution.
 */

#include <stdlib.h>
#ifdef MEMCPY
#include <mem.h>
#endif

#include "zipio.h"
#include "inflate.h"
#include "crc.h"

/*
 * Macros for constants
 */

#ifndef NULL
#define NULL ((void *) 0)
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef ZIPSIGNATURE
#define ZIPSIGNATURE     0x04034b50L
#endif

/*
 * Buffer size macros
 *
 * The following constants are optimized for large-model
 * (but not flat model) Windows with virtual memory.  It
 * will work fine on unix and flat model Windows as well.
 *
 * The constant BUFFERTHRESHOLD determines when memory
 * buffering changes to file buffering.
 *
 * Assumptions:
 *
 *   1) INPBUFSIZE + OUTBUFSIZE + sizeof(void *) * PTRBUFSIZE + delta < 64K
 *
 *   2) OUTBUFSIZE = 32K * N (related to inflate's 32K window size)
 *
 *   2) Max in-memory file size is OUTBUFSIZE * PTRBUFSIZE
 *      which is 64 MBytes by default (32K * 2K).
 *
 */

#ifndef BUFFERTHRESHOLD
#define BUFFERTHRESHOLD            (256 * 1024L)
#endif

#ifndef INPBUFSIZE
#define INPBUFSIZE                 (  8 * 1024 )
#endif

#ifndef PTRBUFSIZE
#define PTRBUFSIZE                 (  2 * 1024 )
#endif

#ifndef OUTBUFSIZE
#define OUTBUFSIZE ((unsigned int) ( 32 * 1024L))
#endif

#define MAXFILESIZE (OUTBUFSIZE * (long) PTRBUFSIZE)

/*
 * Macro for short-hand reference to ZipioState (from ZFILE *)
 */

#define ZS ((struct ZipioState *) stream)

/*
 * Macro for common usage of fseek/fread and fseek/fwrite
 */
#define FREAD(fil, off, buf, len)                         \
  ((fseek((fil), (off), SEEK_SET)                  ) ||   \
   (fread((buf), 1, (size_t) (len), (fil)) != (len))    )

#define FWRITE(fil, off, buf, len)                         \
  ((fseek((fil), (off), SEEK_SET)                   ) ||   \
   (fwrite((buf), 1, (size_t) (len), (fil)) != (len))    )

/*
 * Macros to manipulate zgetc() cache
 */

#define CACHEINIT                                 \
  ZS->ptr = NULL;                                 \
  ZS->len = 0;

#define CACHEUPDATE                               \
  if (ZS->ptr)                                    \
  {                                               \
    ZS->fileposition &= ~((long) (OUTBUFSIZE-1)); \
    ZS->fileposition += ZS->ptr - ZS->getbuf;     \
    ZS->ptr = NULL;                               \
  }                                               \
  ZS->len = 0;

/*
 * Macros for run-time type identification
 */

#ifndef RUNTIMEENABLE
#define RUNTIMEENABLE 0
#endif

#if RUNTIMEENABLE
#define RUNTIMETYPE   0x0110f00fL
#define RUNTIMEDEFINE1                                       \
  unsigned long  runtimetypeid1;
#define RUNTIMEDEFINE2                                       \
  unsigned long  runtimetypeid2;
#define RUNTIMEINIT                                          \
  zs->runtimetypeid1 = RUNTIMETYPE;                          \
  zs->runtimetypeid2 = RUNTIMETYPE;
#define RUNTIMECHECK                                         \
  if (!ZS || (ZS->runtimetypeid1 != RUNTIMETYPE)             \
          || (ZS->runtimetypeid2 != RUNTIMETYPE)) return -1;
#else
#define RUNTIMEDEFINE1
#define RUNTIMEDEFINE2
#define RUNTIMEINIT
#define RUNTIMECHECK
#endif

/*
 * Macros for converting bytes to unsigned integers
 */

#define GETUINT4(ptr, i4)                                               \
  {                                                                     \
    i4 = (((unsigned long) *(((unsigned char *) (ptr)) + 0))      ) |   \
         (((unsigned long) *(((unsigned char *) (ptr)) + 1)) <<  8) |   \
         (((unsigned long) *(((unsigned char *) (ptr)) + 2)) << 16) |   \
         (((unsigned long) *(((unsigned char *) (ptr)) + 3)) << 24)   ; \
  }

#define GETUINT2(ptr, i2)                                               \
  {                                                                     \
    i2 = (((unsigned  int) *(((unsigned char *) (ptr)) + 0))      ) |   \
         (((unsigned  int) *(((unsigned char *) (ptr)) + 1)) <<  8)   ; \
  }

/* Structure to hold state for decoding zip files */
struct ZipioState {

  /* Fields overlaid with ZFILE structure */
  int            len;                        /* length of zgetc cache      */
  unsigned char *ptr;                        /* pointer to zgetc cache     */

  /* Fields invisible to users of ZFILE structure */

  /* Error detection state */
  RUNTIMEDEFINE1                             /* to detect run-time errors  */
  int            errorencountered;           /* error encountered flag     */

  /* Buffering state */
  unsigned char  inpbuf[INPBUFSIZE];         /* inp buffer from zip file   */
  unsigned char *ptrbuf[PTRBUFSIZE];         /* pointers to in-memory bufs */

  unsigned char  getbuf[OUTBUFSIZE];         /* buffer for use by zgetc    */
  long           getoff;                     /* starting offset of getbuf  */

  FILE          *tmpfil;                     /* file ptr to temp file      */

  /* Amount of input and output inflated */
  unsigned long  inpinf;
  unsigned long  outinf;

  /* Offset into file for header and data areas */
  unsigned long  hoff;     /* Offset into OpenFile of header */
  unsigned long  doff;     /* Offset into OpenFile of data   */

  /* Zip file header */
  unsigned long  sign;     /* local file header signature (0x04034b50) */
  unsigned int   vers;     /* version needed to extract       2 bytes  */
  unsigned int   flag;     /* general purpose bit flag        2 bytes  */
  unsigned int   comp;     /* compression method              2 bytes  */
  unsigned int   mtim;     /* last mod file time              2 bytes  */
  unsigned int   mdat;     /* last mod file date              2 bytes  */
  unsigned long  crc3;     /* crc-32                          4 bytes  */
  unsigned long  csiz;     /* compressed size                 4 bytes  */
  unsigned long  usiz;     /* uncompressed size               4 bytes  */
  unsigned int   flen;     /* filename length                 2 bytes  */
  unsigned int   elen;     /* extra field length              2 bytes  */

  char          *name;     /* pointer to file name                     */

  /* Application state */
  FILE          *OpenFile;                   /* currently open file        */

  void          *inflatestate;               /* current state for inflate  */

  unsigned long  fileposition;               /* current file position      */

  unsigned long  filecrc;                    /* current crc                */

  RUNTIMEDEFINE2                             /* to detect run-time errors  */
};

/*
 * Utility routines to handle uncompressed file buffers
 */

/* Initialize buffering (csiz and usiz already set up) */
static void BufferInitialize(
  struct ZipioState *zs,
  int doinflate
)
{
  zs->getoff = -1;
  zs->tmpfil = NULL;

  /*
   * If not inflating, use the input file
   */

  if (!doinflate)
  {
    zs->tmpfil = zs->OpenFile;

    /* Get the uncompressed file size */
    zs->outinf = zs->usiz;
  }

  /* If there's no file open, see if it's big enough for temp file */
  if (!zs->tmpfil)
  {
    if (zs->usiz >= BUFFERTHRESHOLD)
       tmpfile_s(&zs->tmpfil);
  }

  /* If there's no file open, then use memory buffering */
  if (!zs->tmpfil)
  {
    int i;

    for (i=0; i<PTRBUFSIZE; i++)
      zs->ptrbuf[i] = NULL;
  }
}

/* pump data till length bytes of file are inflated or error encountered */
static int BufferPump(struct ZipioState *zs, long length)
{
  size_t inplen;

  /* Check to see if the length is valid */
  if (length > (long)zs->usiz) return TRUE;

  /* Loop till enough data is pumped */
  while (!zs->errorencountered && ((long)zs->outinf < length))
  {
    /* Compute how much data to read */
    if ((zs->csiz - zs->inpinf) < INPBUFSIZE)
      inplen = (size_t) (zs->csiz - zs->inpinf);
    else
      inplen = INPBUFSIZE;

    if (inplen <= 0) return TRUE;

    /* Read some data from the file */
    if (FREAD(zs->OpenFile, zs->doff+zs->inpinf, zs->inpbuf, inplen))
      return TRUE;

    /* Update how much data has been read from the file */
    zs->inpinf += inplen;

    /* Pump this data into the decompressor */
    if (InflatePutBuffer(zs->inflatestate, zs->inpbuf, inplen))
      return TRUE;
  }

  return FALSE;
}

/* Read from the buffer */
static int BufferRead(
  struct ZipioState *zs,
  long offset,
  unsigned char *buffer,
  long length
)
{
  /*
   * Make sure enough bytes have been inflated
   * Note that the correction for reading past EOF has to
   * be done before calling this routine
   */

  if (BufferPump(zs, offset+length)) return TRUE;

  /* If using file buffering, just get the data from the file */
  if (zs->tmpfil)
  {
    long dataoffset;

    if (zs->tmpfil == zs->OpenFile)
      dataoffset = zs->doff;
    else
      dataoffset = 0;

    if (FREAD(zs->tmpfil, dataoffset+offset, buffer, length))
      return TRUE;
  }
  /* If no temp file, use memory buffering */
  else
  {
    unsigned int i;
    unsigned int off, len;
    unsigned char *ptr;

    long           tmpoff;
    unsigned char *tmpbuf;
    long           tmplen;

    /* Save copies of offset, buffer and length for the loop */
    tmpoff = offset;
    tmpbuf = buffer;
    tmplen = length;

    /* Validate the transfer */
    if (tmpoff+tmplen > MAXFILESIZE) return TRUE;

    /* Loop till done */
    while (tmplen)
    {
      /* Get a pointer to the next block */
      i = (unsigned int) (tmpoff / OUTBUFSIZE);
      ptr = zs->ptrbuf[i];
      if (!ptr) return TRUE;

      /* Get the offset,length for this block */
      off = (unsigned int) (tmpoff & (OUTBUFSIZE-1));
      len = OUTBUFSIZE - off;
      if (len > (unsigned int)tmplen) len = (unsigned int) tmplen;

      /* Get the starting pointer for the transfer */
      ptr += off;

      /* Copy the data for this block */
#ifdef MEMCPY
      memcpy(tmpbuf, ptr, len);
#else
      for (i=0; i<len; i++)
        tmpbuf[i] = ptr[i];
#endif

      /* Update the offset, buffer, and length */
      tmpoff += len;
      tmpbuf += len;
      tmplen -= len;
    }
  }

  /* return success */
  return FALSE;
}

/* Append to the buffer */
static int BufferAppend(
  struct ZipioState *zs,
  unsigned char *buffer,
  long length
)
{
  /* If using file buffering, just append the data from the file */
  if (zs->tmpfil)
  {
    if (FWRITE(zs->tmpfil, zs->outinf, buffer, length))
      return TRUE;
  }
  /* If no temp file, use memory buffering */
  else
  {
    unsigned int i;
    unsigned int off, len;
    unsigned char *ptr;

    long           tmpoff;
    unsigned char *tmpbuf;
    long           tmplen;

    /* Save copies of outinf, buffer and length for the loop */
    tmpoff = zs->outinf;
    tmpbuf = buffer;
    tmplen = length;

    /* Validate the transfer */
    if (tmpoff+tmplen > MAXFILESIZE) return TRUE;

    /* Loop till done */
    while (tmplen)
    {
      /* Get a pointer to the next block */
      i = (unsigned int) (tmpoff / OUTBUFSIZE);
      ptr = zs->ptrbuf[i];
      if (!ptr)
      {
        ptr = (unsigned char *) malloc(OUTBUFSIZE);
        if (!ptr) return TRUE;
        zs->ptrbuf[i] = ptr;
      }

      /* Get the offset,length for this block */
      off = (unsigned int) (tmpoff & (OUTBUFSIZE-1));
      len = OUTBUFSIZE - off;
      if (len > (unsigned int)tmplen) len = (unsigned int) tmplen;

      /* Get the starting pointer for the transfer */
      ptr += off;

      /* Copy the data for this block */
#ifdef MEMCPY
      memcpy(ptr, tmpbuf, len);
#else
      for (i=0; i<len; i++)
        ptr[i] = tmpbuf[i];
#endif

      /* Update the offset, buffer, and length */
      tmpoff += len;
      tmpbuf += len;
      tmplen -= len;
    }
  }

  /* Update the output buffer length */
  zs->outinf += length;

  /* return success */
  return FALSE;
}

/* Terminate buffering */
static void BufferTerminate(
  struct ZipioState *zs
)
{
  /* If reading directly from the uncompressed file, just mark with NULL */
  if (zs->tmpfil == zs->OpenFile)
  {
    zs->tmpfil = NULL;
  }
  /* If using the a temporary file, close it */
  else if (zs->tmpfil)
  {
    fclose(zs->tmpfil);
    zs->tmpfil = NULL;
  }
  /* If doing memory buffering, free the buffers */
  else
  {
    int i;

    for (i=0; i<PTRBUFSIZE; i++)
      if (zs->ptrbuf[i]) free(zs->ptrbuf[i]);
  }
}

/*
 * callout routines for InflateInitialize
 */

static int inflate_putbuffer(             /* returns 0 on success       */
    void *stream,                         /* opaque ptr from Initialize */
    unsigned char *buffer,                /* buffer to put              */
    long length                           /* length of buffer           */
)
{
  RUNTIMECHECK;

  /* If the write will go past the end of file, return an error */
  if (ZS->outinf + length > ZS->usiz) return TRUE;

  /* Update the CRC */
  ZS->filecrc = CrcUpdate(ZS->filecrc, buffer, length);

  /* Append to the buffer */
  if (BufferAppend(ZS, buffer, length)) return TRUE;

  /* Return success */
  return FALSE;
}

static void *inflate_malloc(long length)
{
  return malloc((size_t) length);
}

static void inflate_free(void *buffer)
{
  free(buffer);
}

static void zload(ZFILE *stream, unsigned long off)
{
  /* Set up the initial values of the inflate state */

  CACHEINIT;

  RUNTIMEINIT;

  ZS->errorencountered = FALSE;

  ZS->inpinf           = 0;
  ZS->outinf           = 0;

  ZS->fileposition     = 0;

  ZS->filecrc          = 0xffffffffL;

  ZS->name             = NULL;

  /* Set up the header offset */
  ZS->hoff = off;

  /* Read the first input buffer */
  if (FREAD(ZS->OpenFile, ZS->hoff, ZS->inpbuf, 30))
  {
    ZS->sign = 0;
  }
  else
  {
    GETUINT4(ZS->inpbuf+ 0, ZS->sign);
    GETUINT2(ZS->inpbuf+ 4, ZS->vers);
    GETUINT2(ZS->inpbuf+ 6, ZS->flag);
    GETUINT2(ZS->inpbuf+ 8, ZS->comp);
    GETUINT2(ZS->inpbuf+10, ZS->mtim);
    GETUINT2(ZS->inpbuf+12, ZS->mdat);
    GETUINT4(ZS->inpbuf+14, ZS->crc3);
    GETUINT4(ZS->inpbuf+18, ZS->csiz);
    GETUINT4(ZS->inpbuf+22, ZS->usiz);
    GETUINT2(ZS->inpbuf+26, ZS->flen);
    GETUINT2(ZS->inpbuf+28, ZS->elen);

#ifdef PRINTZIPHEADER
    fprintf(stderr, "local file header signature  hex %8lx\n", ZS->sign);
    fprintf(stderr, "version needed to extract        %8d\n" , ZS->vers);
    fprintf(stderr, "general purpose bit flag     hex %8x\n" , ZS->flag);
    fprintf(stderr, "compression method               %8d\n" , ZS->comp);
    fprintf(stderr, "last mod file time               %8d\n" , ZS->mtim);
    fprintf(stderr, "last mod file date               %8d\n" , ZS->mdat);
    fprintf(stderr, "crc-32                       hex %8lx\n", ZS->crc3);
    fprintf(stderr, "compressed size                  %8ld\n", ZS->csiz);
    fprintf(stderr, "uncompressed size                %8ld\n", ZS->usiz);
    fprintf(stderr, "filename length                  %8d\n" , ZS->flen);
    fprintf(stderr, "extra field length               %8d\n" , ZS->elen);
#endif
  }

  /*
   * If the file isn't a zip file, set up to read it normally
   */
  if (ZS->sign != ZIPSIGNATURE)
  {
    /* Only handle uncompressed data at the beginning of the file */
    if (ZS->hoff != 0)
    {
      ZS->errorencountered = TRUE;
      return;
    }

    /* Compute the offset to the data area */
    ZS->doff = 0;

    /* Set up the usiz and csiz fields to reflect true file size */
    fseek(ZS->OpenFile, 0, SEEK_END);
    ZS->usiz   = ftell(ZS->OpenFile);
    ZS->csiz   = ZS->usiz;

    /* Initialize buffering */
    BufferInitialize(ZS, FALSE);

    /* Don't use the decompression routines */
    ZS->inflatestate = NULL;
  }
  else
  {
    /* Allocate and read a new name */
    if (ZS->flen > 0)
    {
      ZS->name = (char *) malloc(ZS->flen+1);
      if (ZS->name)
      {
        if (FREAD(ZS->OpenFile, ZS->hoff+30, ZS->name, ZS->flen))
        {
          free(ZS->name);
          ZS->name = NULL;
          ZS->errorencountered = TRUE;
          return;
        }
        ZS->name[ZS->flen] = 0;
      }
    }

    /* Compute the offset to the data area */
    ZS->doff = ZS->hoff + 30 + ZS->flen + ZS->elen;

    /* If the data is encrypted, can't read it */
    if (ZS->flag & 1)
    {
      ZS->errorencountered = TRUE;
      return;
    }

    /* Handle compression type 8 (deflated) */
    if (ZS->comp == 8)
    {
      /* Initialize buffering */
      BufferInitialize(ZS, TRUE);

      /* Initialize the decompression routines */
      ZS->inflatestate = InflateInitialize(
                           (void *) ZS,
                           inflate_putbuffer,
                           inflate_malloc,
                           inflate_free
                         );
    }
    /* Handle compression type 0 (stored) */
    else if (ZS->comp == 0)
    {
      /* Initialize buffering */
      BufferInitialize(ZS, FALSE);

      /* Don't use the decompression routines */
      ZS->inflatestate = NULL;
    }
    /* Return error for other compression types */
    else
    {
      ZS->errorencountered = TRUE;
    }
  }
}

static void zdone(ZFILE *stream)
{
  /* Free any existing file name */
  if (ZS->name)
  {
    free(ZS->name);
    ZS->name = NULL;
  }

  /* terminate the inflate routines, and check for errors */
  if (ZS->inflatestate)
  {
    if (InflateTerminate(ZS->inflatestate))
      ZS->errorencountered = TRUE;

    /* Check that the CRC is OK if we've read to the end */
    if (ZS->inpinf >= ZS->csiz)
    {
      if (ZS->filecrc != (ZS->crc3 ^ 0xffffffffL))
        ZS->errorencountered = TRUE;
    }
  }

  /* terminate the buffering */
  BufferTerminate(ZS);
}

ZFILE *zopen(const char *path, const char *mode)
{
  struct ZipioState *zs;

  /* Allocate the ZipioState memory area */
  zs = (struct ZipioState *) malloc(sizeof(struct ZipioState));
  if (!zs) return NULL;

  /* Open the real file */
  if (fopen_s(&zs->OpenFile, path, mode))
  {
    free(zs);
    return NULL;
  }

  /* Load the header and figure out what kind of file it is */
  zload((ZFILE *) zs, 0);

  /* Return this state info to the caller */
  return (ZFILE *) zs;
}

int _zgetc(ZFILE *stream)
{
  long offset, length;

  int off;

  RUNTIMECHECK;

  if (ZS->errorencountered) return -1;

  CACHEUPDATE;

  /* If already at EOF, return */
  if (ZS->fileposition >= ZS->usiz) return -1;

  /* If data isn't in current outbuf, get it */
  offset = ZS->fileposition & ~((long) (OUTBUFSIZE-1));
  if (ZS->getoff != offset)
  {
    length = ZS->usiz - offset;
    if (length > OUTBUFSIZE) length = OUTBUFSIZE;

    if (BufferRead(ZS, offset, ZS->getbuf, length)) return -1;

    ZS->getoff = offset;
  }

  /* Set up the cache */
  off = (int) (ZS->fileposition & (OUTBUFSIZE-1));
  ZS->len = (int) (length - off);
  ZS->ptr = ZS->getbuf    + off;

  /* Return the character */
           ZS->len--;
  return *(ZS->ptr++);
}

size_t zread(void *ptr, size_t size, size_t n, ZFILE *stream)
{
  long           length;

  RUNTIMECHECK;

  if (ZS->errorencountered) return 0;

  CACHEUPDATE;

  /* Compute the length requested */
  length = size * (long) n;

  /* Adjust the length to account for premature EOF */
  if (ZS->fileposition+length > ZS->usiz)
    length = ZS->usiz - ZS->fileposition;

  /* If the length is zero, then just return an EOF error */
  if (length <= 0) return 0;

  /* Make the length a multiple of size */
  length /= size;
  length *= size;

  /* If the length is zero, then just return an EOF error */
  if (length <= 0) return 0;

  /* Read from the buffer */
  if (BufferRead(ZS, ZS->fileposition, (unsigned char *) ptr, length))
    return 0;

  /* Update the file position */
  ZS->fileposition += length;

  /* Return the number of items transferred */
  return (size_t) (length / size);
}

int zseek(ZFILE *stream, long offset, int whence)
{
  long newoffset;

  RUNTIMECHECK;

  if (ZS->errorencountered) return -1;

  CACHEUPDATE;

  if (whence == SEEK_SET)
  {
    newoffset = offset;
  }
  else if (whence == SEEK_CUR)
  {
    newoffset = ZS->fileposition + offset;
  }
  else if (whence == SEEK_END)
  {
    newoffset = ZS->fileposition + ZS->usiz;
  }
  else
  {
    return -1;
  }

  if ((newoffset < 0) || (newoffset > (long)ZS->usiz)) return -1;

  ZS->fileposition = newoffset;

  return 0;
}

long ztell(ZFILE *stream)
{
  RUNTIMECHECK;

  if (ZS->errorencountered) return -1;

  CACHEUPDATE;

  return ZS->fileposition;
}

int zclose(ZFILE *stream)
{
  int ret;

  RUNTIMECHECK;

  CACHEUPDATE;

  zdone(stream);

  /* save the final error status */
  ret = ZS->errorencountered;

  /* Close the file */
  if (ZS->OpenFile) fclose(ZS->OpenFile);

  /* free the ZipioState structure */
  free(ZS);

  /* return the final error status */
  return ret;
}

/* Return the filename of the current file within the zip archive */
char *zname(ZFILE *stream)
{
  RUNTIMECHECK;

  return ZS->name;
}

/* Return the error status of the current file within the zip archive */
int zerror(ZFILE *stream)
{
  RUNTIMECHECK;

  CACHEUPDATE;

  if (ZS->errorencountered)
    return -1;
  else
    return  0;
}

/* Advance to the next file within the zip archive, err if no more */
int znext(ZFILE *stream)
{
  RUNTIMECHECK;

  CACHEUPDATE;

  zdone(stream);

  zload(stream, ZS->doff + ZS->csiz);

  if (!ZS->name)
    return -1;
  else
    return  0;
}

int	zIsZIP(ZFILE *stream)
{
	return (ZS->sign == ZIPSIGNATURE);
}
