/*
 * crc.c - CRC calculation routine
 *
 * Version 1.1
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
 * Generate a table for a byte-wise 32-bit CRC calculation on the polynomial:
 * x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x+1.
 *
 * Polynomials over GF(2) are represented in binary, one bit per coefficient,
 * with the lowest powers in the most significant bit.  Then adding polynomials
 * is just exclusive-or, and multiplying a polynomial by x is a right shift by
 * one.  If we call the above polynomial p, and represent a byte as the
 * polynomial q, also with the lowest power in the most significant bit (so the
 * byte 0xb1 is the polynomial x^7+x^3+x+1), then the CRC is (q*x^32) mod p,
 * where a mod b means the remainder after dividing a by b.
 *
 * This calculation is done using the shift-register method of multiplying and
 * taking the remainder.  The register is initialized to zero, and for each
 * incoming bit, x^32 is added mod p to the register if the bit is a one (where
 * x^32 mod p is p+x^32 = x^26+...+1), and the register is multiplied mod p by
 * x (which is shifting right by one and adding x^32 mod p if the bit shifted
 * out is a one).  We start with the highest power (least significant bit) of
 * q and repeat for all eight bits of q.
 *
 * The table is simply the CRC of all possible eight bit values.  This is all
 * the information needed to generate CRC's on data a byte at a time for all
 * combinations of CRC register values and incoming bytes.  The table is
 * written to stdout as 256 long hexadecimal values in C language format.
 *
 * The computation of the CRC table is from public domain source code
 * written by Mark Adler.
 */

#include <stdlib.h>
#include "crc.h"

static unsigned long *crctable = NULL;

/*
 * This CRC algorithm is the same as that used in zip.  Normally it
 * should be initialized with 0xffffffff, and the final CRC stored
 * should be crc ^ 0xffffffff.
 */

unsigned long CrcUpdate(          /* returns updated crc         */
  unsigned long crc,              /* starting crc                */
  unsigned char *buffer,          /* buffer to use to update crc */
  long length                     /* length of buffer            */
)
{
  /* Compute the crc table if not yet initialized */
  if (!crctable)
  {
    crctable = (unsigned long *) malloc(256 * sizeof(unsigned long));
  
    if (crctable)
    {
      unsigned long shf;   /* crc shift register                        */
      int i;               /* counter for all possible eight bit values */
      int k;               /* counter for bit being shifted into crc    */
  
      for (i=0; i<256; i++)
      {
        shf = i;
        for (k=0; k<8; k++)
        {
          shf = (shf & 1) ? ((shf >> 1) ^ 0xedb88320L) 
                          :  (shf >> 1)                ;
        }
        crctable[i] = shf;
      }
    }
  }
  
  /* Update the crc with each byte */
  if (crctable)
  {
    long i;

    for (i=0; i<length; i++)
    {
      crc = crctable[buffer[i] ^ ((unsigned char) crc)] ^ (crc >> 8);
    }
  }

  return crc;
}
