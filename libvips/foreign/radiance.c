/* Read Radiance (.hdr) files 
 *
 * 3/3/09
 * 	- write packed data, a separate im_rad2float() operation can unpack
 * 23/3/09
 * 	- add radiance write
 * 20/12/11
 * 	- reworked as some fns ready for new-style classes
 * 13/12/12
 * 	- tag RGB rad images as scRGB
 * 4/11/13
 * 	- support sequential read
 * 5/11/13
 * 	- rewritten scanline encode and decode, now much faster
 * 23/1/14
 * 	- put the reader globals into a struct so we can have many active
 * 	  readers
 * 23/5/16
 *	- add buffer save functions   
 * 28/2/17
 * 	- use dbuf for buffer output
 * 4/4/17
 * 	- reduce stack use to help musl
 * 22/7/18
 * 	- update code from radiance ... pasted in from rad5R1
 * 	- expand fs[] buffer to prevent out of bounds write [HongxuChen]
 * 23/7/18
 * 	- fix a buffer overflow for incorrectly coded old-style RLE
 * 	  [HongxuChen]
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*

	Remaining issues:

+ it ignores some header fields, like VIEW and DATE

+ it will not rotate/flip as the FORMAT string asks

 */

/*

    Sections of this reader from Greg Ward and Radiance with kind 
    permission. The Radience copyright notice appears below.

 */

/* ====================================================================
 * The Radiance Software License, Version 1.0
 *
 * Copyright (c) 1990 - 2009 The Regents of the University of California,
 * through Lawrence Berkeley National Laboratory.   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *           if any, must include the following acknowledgment:
 *             "This product includes Radiance software
 *                 (http://radsite.lbl.gov/)
 *                 developed by the Lawrence Berkeley National Laboratory
 *               (http://www.lbl.gov/)."
 *       Alternately, this acknowledgment may appear in the software itself,
 *       if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Radiance," "Lawrence Berkeley National Laboratory"
 *       and "The Regents of the University of California" must
 *       not be used to endorse or promote products derived from this
 *       software without prior written permission. For written
 *       permission, please contact radiance@radsite.lbl.gov.
 *
 * 5. Products derived from this software may not be called "Radiance",
 *       nor may "Radiance" appear in their name, without prior written
 *       permission of Lawrence Berkeley National Laboratory.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.   IN NO EVENT SHALL Lawrence Berkeley National Laboratory OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of Lawrence Berkeley National Laboratory.   For more
 * information on Lawrence Berkeley National Laboratory, please see
 * <http://www.lbl.gov/>.
 */

/*
#define DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#ifdef HAVE_RADIANCE

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <vips/vips.h>
#include <vips/internal.h>
#include <vips/debug.h>

#include "pforeign.h"

/* Begin copy-paste from Radiance sources.
 *
 * To update:
 *
 * 1. Download and unpack latest stable radiance
 * 2. ray/src/common has the files we need ... copy in this order:
 * 	colour.h
 * 	resolu.h
 * 	rtio.h
 * 	fputword.c
 * 	colour.c
 * 	resolu.c
 * 	header.c
 * 3. trim each one down, removing extern decls
 * 4. make all functions static
 * 5. reorder to remove forward refs
 * 6. remove unused funcs, mostly related to HDR write
 */

#define  RED		0
#define  GRN		1
#define  BLU		2
#define  CIEX		0	/* or, if input is XYZ... */
#define  CIEY		1
#define  CIEZ		2
#define  EXP		3	/* exponent same for either format */
#define  COLXS		128	/* excess used for exponent */
#define  WHT		3	/* used for RGBPRIMS type */

#undef uby8
#define uby8  unsigned char	/* 8-bit unsigned integer */

typedef uby8  COLR[4];		/* red, green, blue (or X,Y,Z), exponent */

typedef float COLORV;
typedef COLORV  COLOR[3];	/* red, green, blue (or X,Y,Z) */

typedef float  RGBPRIMS[4][2];	/* (x,y) chromaticities for RGBW */
typedef float  (*RGBPRIMP)[2];	/* pointer to RGBPRIMS array */

typedef float  COLORMAT[3][3];	/* color coordinate conversion matrix */

#define  copycolr(c1,c2)	(c1[0]=c2[0],c1[1]=c2[1], \
				c1[2]=c2[2],c1[3]=c2[3])

#define  colval(col,pri)	((col)[pri])

#define  setcolor(col,r,g,b)	((col)[RED]=(r),(col)[GRN]=(g),(col)[BLU]=(b))

#define  copycolor(c1,c2)	((c1)[0]=(c2)[0],(c1)[1]=(c2)[1],(c1)[2]=(c2)[2])

#define  scalecolor(col,sf)	((col)[0]*=(sf),(col)[1]*=(sf),(col)[2]*=(sf))

#define  addcolor(c1,c2)	((c1)[0]+=(c2)[0],(c1)[1]+=(c2)[1],(c1)[2]+=(c2)[2])

#define  multcolor(c1,c2)	((c1)[0]*=(c2)[0],(c1)[1]*=(c2)[1],(c1)[2]*=(c2)[2])

#ifdef  NTSC
#define  CIE_x_r		0.670		/* standard NTSC primaries */
#define  CIE_y_r		0.330
#define  CIE_x_g		0.210
#define  CIE_y_g		0.710
#define  CIE_x_b		0.140
#define  CIE_y_b		0.080
#define  CIE_x_w		(1./3.)		/* use true white */
#define  CIE_y_w		(1./3.)
#else
#define  CIE_x_r		0.640		/* nominal CRT primaries */
#define  CIE_y_r		0.330
#define  CIE_x_g		0.290
#define  CIE_y_g		0.600
#define  CIE_x_b		0.150
#define  CIE_y_b		0.060
#define  CIE_x_w		(1./3.)		/* use true white */
#define  CIE_y_w		(1./3.)
#endif

#define  STDPRIMS	{{CIE_x_r,CIE_y_r},{CIE_x_g,CIE_y_g}, \
				{CIE_x_b,CIE_y_b},{CIE_x_w,CIE_y_w}}

#define CIE_D		(	CIE_x_r*(CIE_y_g - CIE_y_b) + \
				CIE_x_g*(CIE_y_b - CIE_y_r) + \
				CIE_x_b*(CIE_y_r - CIE_y_g)	)
#define CIE_C_rD	( (1./CIE_y_w) * \
				( CIE_x_w*(CIE_y_g - CIE_y_b) - \
				  CIE_y_w*(CIE_x_g - CIE_x_b) + \
				  CIE_x_g*CIE_y_b - CIE_x_b*CIE_y_g	) )
#define CIE_C_gD	( (1./CIE_y_w) * \
				( CIE_x_w*(CIE_y_b - CIE_y_r) - \
				  CIE_y_w*(CIE_x_b - CIE_x_r) - \
				  CIE_x_r*CIE_y_b + CIE_x_b*CIE_y_r	) )
#define CIE_C_bD	( (1./CIE_y_w) * \
				( CIE_x_w*(CIE_y_r - CIE_y_g) - \
				  CIE_y_w*(CIE_x_r - CIE_x_g) + \
				  CIE_x_r*CIE_y_g - CIE_x_g*CIE_y_r	) )

#define CIE_rf		(CIE_y_r*CIE_C_rD/CIE_D)
#define CIE_gf		(CIE_y_g*CIE_C_gD/CIE_D)
#define CIE_bf		(CIE_y_b*CIE_C_bD/CIE_D)

/* As of 9-94, CIE_rf=.265074126, CIE_gf=.670114631 and CIE_bf=.064811243 */

/***** The following definitions are valid for RGB colors only... *****/

#define  bright(col)	(CIE_rf*(col)[RED]+CIE_gf*(col)[GRN]+CIE_bf*(col)[BLU])
#define  normbright(c)	( ( (long)(CIE_rf*256.+.5)*(c)[RED] + \
			    (long)(CIE_gf*256.+.5)*(c)[GRN] + \
			    (long)(CIE_bf*256.+.5)*(c)[BLU] ) >> 8 )

				/* luminous efficacies over visible spectrum */
#define  MAXEFFICACY		683.		/* defined maximum at 550 nm */
#define  WHTEFFICACY		179.		/* uniform white light */
#define  D65EFFICACY		203.		/* standard illuminant D65 */
#define  INCEFFICACY		160.		/* illuminant A (incand.) */
#define  SUNEFFICACY		208.		/* illuminant B (solar dir.) */
#define  SKYEFFICACY		D65EFFICACY	/* skylight (should be 110) */
#define  DAYEFFICACY		D65EFFICACY	/* combined sky and solar */

#define  luminance(col)		(WHTEFFICACY * bright(col))

/***** ...end of stuff specific to RGB colors *****/

#define  intens(col)		( (col)[0] > (col)[1] \
				? (col)[0] > (col)[2] ? (col)[0] : (col)[2] \
				: (col)[1] > (col)[2] ? (col)[1] : (col)[2] )

#define  colrval(c,p)		( (c)[EXP] ? \
				ldexp((c)[p]+.5,(int)(c)[EXP]-(COLXS+8)) : \
				0. )

#define  WHTCOLOR		{1.0,1.0,1.0}
#define  BLKCOLOR		{0.0,0.0,0.0}
#define  WHTCOLR		{128,128,128,COLXS+1}
#define  BLKCOLR		{0,0,0,0}

				/* picture format identifier */
#define  COLRFMT		"32-bit_rle_rgbe"
#define  CIEFMT			"32-bit_rle_xyze"
#define  PICFMT			"32-bit_rle_???e"	/* matches either */
#define  LPICFMT		15			/* max format id len */

				/* macros for exposures */
#define  EXPOSSTR		"EXPOSURE="
#define  LEXPOSSTR		9
#define  isexpos(hl)		(!strncmp(hl,EXPOSSTR,LEXPOSSTR))
#define  exposval(hl)		atof((hl)+LEXPOSSTR)
#define  fputexpos(ex,fp)	fprintf(fp,"%s%e\n",EXPOSSTR,ex)

				/* macros for pixel aspect ratios */
#define  ASPECTSTR		"PIXASPECT="
#define  LASPECTSTR		10
#define  isaspect(hl)		(!strncmp(hl,ASPECTSTR,LASPECTSTR))
#define  aspectval(hl)		atof((hl)+LASPECTSTR)
#define  fputaspect(pa,fp)	fprintf(fp,"%s%f\n",ASPECTSTR,pa)

				/* macros for primary specifications */
#define  PRIMARYSTR		"PRIMARIES="
#define  LPRIMARYSTR		10
#define  isprims(hl)		(!strncmp(hl,PRIMARYSTR,LPRIMARYSTR))
#define  primsval(p,hl)		(sscanf((hl)+LPRIMARYSTR, \
					"%f %f %f %f %f %f %f %f", \
					&(p)[RED][CIEX],&(p)[RED][CIEY], \
					&(p)[GRN][CIEX],&(p)[GRN][CIEY], \
					&(p)[BLU][CIEX],&(p)[BLU][CIEY], \
					&(p)[WHT][CIEX],&(p)[WHT][CIEY]) == 8)
#define  fputprims(p,fp)	fprintf(fp, \
				"%s %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",\
					PRIMARYSTR, \
					(p)[RED][CIEX],(p)[RED][CIEY], \
					(p)[GRN][CIEX],(p)[GRN][CIEY], \
					(p)[BLU][CIEX],(p)[BLU][CIEY], \
					(p)[WHT][CIEX],(p)[WHT][CIEY])

				/* macros for color correction */
#define  COLCORSTR		"COLORCORR="
#define  LCOLCORSTR		10
#define  iscolcor(hl)		(!strncmp(hl,COLCORSTR,LCOLCORSTR))
#define  colcorval(cc,hl)	sscanf((hl)+LCOLCORSTR,"%f %f %f", \
					&(cc)[RED],&(cc)[GRN],&(cc)[BLU])
#define  fputcolcor(cc,fp)	fprintf(fp,"%s %f %f %f\n",COLCORSTR, \
					(cc)[RED],(cc)[GRN],(cc)[BLU])

/*
 * Conversions to and from XYZ space generally don't apply WHTEFFICACY.
 * If you need Y to be luminance (cd/m^2), this must be applied when
 * converting from radiance (watts/sr/m^2).
 */

extern RGBPRIMS  stdprims;	/* standard primary chromaticities */
extern COLORMAT  rgb2xyzmat;	/* RGB to XYZ conversion matrix */
extern COLORMAT  xyz2rgbmat;	/* XYZ to RGB conversion matrix */
extern COLOR  cblack, cwhite;	/* black (0,0,0) and white (1,1,1) */

#define  CGAMUT_LOWER		01
#define  CGAMUT_UPPER		02
#define  CGAMUT			(CGAMUT_LOWER|CGAMUT_UPPER)

#define  rgb_cie(xyz,rgb)	colortrans(xyz,rgb2xyzmat,rgb)

#define  cpcolormat(md,ms)	memcpy((void *)md,(void *)ms,sizeof(COLORMAT))

#ifdef getc_unlocked		/* avoid horrendous overhead of flockfile */
#undef getc
#undef putc
#define getc    getc_unlocked
#define putc    putc_unlocked
#endif

#define  MINELEN	8	/* minimum scanline length for encoding */
#define  MAXELEN	0x7fff	/* maximum scanline length for encoding */
#define  MINRUN		4	/* minimum run length */

			/* flags for scanline ordering */
#define  XDECR			1
#define  YDECR			2
#define  YMAJOR			4

			/* standard scanline ordering */
#define  PIXSTANDARD		(YMAJOR|YDECR)
#define  PIXSTDFMT		"-Y %d +X %d\n"

			/* structure for image dimensions */
typedef struct {
	int	rt;		/* orientation (from flags above) */
	int	xr, yr;		/* x and y resolution */
} RESOLU;

			/* macros to get scanline length and number */
#define  scanlen(rs)		((rs)->rt & YMAJOR ? (rs)->xr : (rs)->yr)
#define  numscans(rs)		((rs)->rt & YMAJOR ? (rs)->yr : (rs)->xr)

			/* resolution string buffer and its size */
#define  RESOLU_BUFLEN		32

			/* macros for reading/writing resolution struct */
#define  fputsresolu(rs,fp)	fputs(resolu2str(resolu_buf,rs),fp)
#define  fgetsresolu(rs,fp)	str2resolu(rs, \
					fgets(resolu_buf,RESOLU_BUFLEN,fp))

			/* reading/writing of standard ordering */
#define  fprtresolu(sl,ns,fp)	fprintf(fp,PIXSTDFMT,ns,sl)
#define  fscnresolu(sl,ns,fp)	(fscanf(fp,PIXSTDFMT,ns,sl)==2)

			/* identify header lines */
#define  isheadid(s)	headidval(NULL,s)
#define  isformat(s)	formatval(NULL,s)
#define  isdate(s)	dateval(NULL,s)
#define  isgmt(s)	gmtval(NULL,s)

#define  LATLONSTR	"LATLONG="
#define  LLATLONSTR	8
#define  islatlon(hl)		(!strncmp(hl,LATLONSTR,LLATLONSTR))
#define  latlonval(ll,hl)	sscanf((hl)+LLATLONSTR, "%f %f", \
						&(ll)[0],&(ll)[1])
#define  fputlatlon(lat,lon,fp)	fprintf(fp,"%s %.6f %.6f\n",LATLONSTR,lat,lon)

typedef int gethfunc(char *s, void *p); /* callback to process header lines */

#ifdef getc_unlocked		/* avoid horrendous overhead of flockfile */
#undef getc
#undef putc
#define getc    getc_unlocked
#define putc    putc_unlocked
#endif


static char  resolu_buf[RESOLU_BUFLEN];	/* resolution line buffer */

static char *
resolu2str(buf, rp)		/* convert resolution struct to line */
char  *buf;
register RESOLU  *rp;
{
	if (rp->rt&YMAJOR)
		sprintf(buf, "%cY %d %cX %d\n",
				rp->rt&YDECR ? '-' : '+', rp->yr,
				rp->rt&XDECR ? '-' : '+', rp->xr);
	else
		sprintf(buf, "%cX %d %cY %d\n",
				rp->rt&XDECR ? '-' : '+', rp->xr,
				rp->rt&YDECR ? '-' : '+', rp->yr);
	return(buf);
}


static int
str2resolu(rp, buf)		/* convert resolution line to struct */
register RESOLU  *rp;
char  *buf;
{
	register char  *xndx, *yndx;
	register char  *cp;

	if (buf == NULL)
		return(0);
	xndx = yndx = NULL;
	for (cp = buf; *cp; cp++)
		if (*cp == 'X')
			xndx = cp;
		else if (*cp == 'Y')
			yndx = cp;
	if (xndx == NULL || yndx == NULL)
		return(0);
	rp->rt = 0;
	if (xndx > yndx) rp->rt |= YMAJOR;
	if (xndx[-1] == '-') rp->rt |= XDECR;
	if (yndx[-1] == '-') rp->rt |= YDECR;
	if ((rp->xr = atoi(xndx+1)) <= 0)
		return(0);
	if ((rp->yr = atoi(yndx+1)) <= 0)
		return(0);
	return(1);
}

#define	 MAXLINE	2048
#define	 MAXFMTLEN	2048

static const char  FMTSTR[] = "FORMAT=";	/* format identifier */


static gethfunc mycheck;


static int
formatval(			/* get format value (return true if format) */
	char fmt[MAXFMTLEN],
	const char  *s
)
{
	const char  *cp = FMTSTR;
	char  *r = fmt;

	while (*cp) if (*cp++ != *s++) return(0);
	while (isspace(*s)) s++;
	if (!*s) return(0);
	if (r == NULL) return(1);
	do
		*r++ = *s++;
	while (*s && !isspace(*s) && r-fmt < MAXFMTLEN-1);
	*r = '\0';
	return(1);
}


static void
fputformat(		/* put out a format value */
	const char  *s,
	FILE  *fp
)
{
	fputs(FMTSTR, fp);
	fputs(s, fp);
	putc('\n', fp);
}


static int
getheader(		/* get header from file */
	FILE  *fp,
	gethfunc *f,
	void  *p
)
{
	int   rtotal = 0;
	char  buf[MAXLINE];

	for ( ; ; ) {
		int	rval = 0;
		buf[MAXLINE-2] = '\n';
		if (fgets(buf, MAXLINE, fp) == NULL)
			return(-1);
		if (buf[buf[0]=='\r'] == '\n')
			return(rtotal);
		if (buf[MAXLINE-2] != '\n') {
			ungetc(buf[MAXLINE-2], fp);	/* prevent false end */
			buf[MAXLINE-2] = '\0';
		}
		if (f != NULL && (rval = (*f)(buf, p)) < 0)
			return(-1);
		rtotal += rval;
	}
}


struct check {
	FILE	*fp;
	char	fs[MAXFMTLEN];
};


static int
mycheck(			/* check a header line for format info. */
	char  *s,
	void  *cp
)
{
	struct check *p = (struct check *) cp;

	if (!formatval(p->fs, s) && p->fp != NULL) {
		fputs(s, p->fp);
	}
	return(0);
}


static int
globmatch(			/* check for match of s against pattern p */
	const char	*p,
	const char	*s
)
{
	int	setmatch;

	do {
		switch (*p) {
		case '?':			/* match any character */
			if (!*s++)
				return(0);
			break;
		case '*':			/* match any string */
			while (p[1] == '*') p++;
			do
				if ( (p[1]=='?') | (p[1]==*s) &&
						globmatch(p+1,s) )
					return(1);
			while (*s++);
			return(0);
		case '[':			/* character set */
			setmatch = *s == *++p;
			if (!*p)
				return(0);
			while (*++p != ']') {
				if (!*p)
					return(0);
				if (*p == '-') {
					setmatch += (p[-1] <= *s && *s <= p[1]);
					if (!*++p)
						break;
				} else
					setmatch += (*p == *s);
			}
			if (!setmatch)
				return(0);
			s++;
			break;
		case '\\':			/* literal next */
			p++;
		/* fall through */
		default:			/* normal character */
			if (*p != *s)
				return(0);
			s++;
			break;
		}
	} while (*p++);
	return(1);
}


/*
 * Checkheader(fin,fmt,fout) returns a value of 1 if the input format
 * matches the specification in fmt, 0 if no input format was found,
 * and -1 if the input format does not match or there is an
 * error reading the header.  If fmt is empty, then -1 is returned
 * if any input format is found (or there is an error), and 0 otherwise.
 * If fmt contains any '*' or '?' characters, then checkheader
 * does wildcard expansion and copies a matching result into fmt.
 * Be sure that fmt is big enough to hold the match in such cases,
 * and that it is not a static, read-only string!
 * The input header (minus any format lines) is copied to fout
 * if fout is not NULL.
 */

static int
checkheader(
	FILE  *fin,
	char  fmt[MAXFMTLEN],
	FILE  *fout
)
{
	struct check	cdat;
	char	*cp;

	cdat.fp = fout;
	cdat.fs[0] = '\0';
	if (getheader(fin, mycheck, &cdat) < 0)
		return(-1);
	if (!cdat.fs[0])
		return(0);
	for (cp = fmt; *cp; cp++)		/* check for globbing */
		if ((*cp == '?') | (*cp == '*')) {
			if (globmatch(fmt, cdat.fs)) {
				strcpy(fmt, cdat.fs);
				return(1);
			} else
				return(-1);
		}
	return(strcmp(fmt, cdat.fs) ? -1 : 1);	/* literal match */
}

/* End copy-paste from Radiance sources.
 */

#define BUFFER_SIZE (4096)
#define BUFFER_MARGIN (256)

/* Read from a FILE with a rolling memory buffer ... this lets us reduce the
 * number of fgetc() and gives us some very quick readahead.
 */

typedef struct _Buffer { 
	unsigned char text[BUFFER_SIZE + BUFFER_MARGIN];
	int length;
	int position;
	FILE *fp;
} Buffer; 

static Buffer *
buffer_new( FILE *fp )
{
	Buffer *buffer = g_new0( Buffer, 1 );

	buffer->length = 0;
	buffer->position = 0;
	buffer->fp = fp;

	return( buffer ); 
}

static void
buffer_free( Buffer *buffer )
{
	g_free( buffer ); 
}

/* Make sure there are at least @require bytes of readahead available.
 */
static int
buffer_need( Buffer *buffer, int require )
{
	int remaining;

	g_assert( require < BUFFER_MARGIN ); 
	g_assert( buffer->length >= 0 ); 
	g_assert( buffer->position >= 0 ); 
	g_assert( buffer->position <= buffer->length ); 

	remaining = buffer->length - buffer->position;
	if( remaining < require ) {
		size_t len;

		/* Areas can overlap.
		 */
		memmove( buffer->text, 
			buffer->text + buffer->position, remaining ); 
		buffer->position = 0;
		buffer->length = remaining;

		g_assert( buffer->length < BUFFER_MARGIN ); 

		len = fread( buffer->text + buffer->length, 
			1, BUFFER_SIZE, buffer->fp );
		buffer->length += len;
		remaining = buffer->length - buffer->position;

		if( remaining < require ) {
			vips_error( "rad2vips", "%s", _( "end of file" ) ); 
			return( -1 );
		}
	}

	return( 0 );
}

#define BUFFER_FETCH(B) ((B)->text[(B)->position++])
#define BUFFER_PEEK(B) ((B)->text[(B)->position])

/* Read a single scanlne, encoded in the old style.
 */
static int
scanline_read_old( Buffer *buffer, COLR *scanline, int width )
{
	int rshift;

	g_assert( buffer->length >= 0 ); 
	g_assert( buffer->position >= 0 ); 
	g_assert( buffer->position <= buffer->length ); 

	rshift = 0;
	
	while( width > 0 ) {
		if( buffer_need( buffer, 4 ) )
			return( -1 ); 

		scanline[0][RED] = BUFFER_FETCH( buffer );
		scanline[0][GRN] = BUFFER_FETCH( buffer );
		scanline[0][BLU] = BUFFER_FETCH( buffer );
		scanline[0][EXP] = BUFFER_FETCH( buffer );

		if( scanline[0][RED] == 1 &&
			scanline[0][GRN] == 1 &&
			scanline[0][BLU] == 1 ) {
			int i;

			for( i = scanline[0][EXP] << rshift; 
				i > 0 && width > 0; i-- ) {
				copycolr( scanline[0], scanline[-1] );
				scanline += 1;
				width -= 1;
			}

			rshift += 8;
		} 
		else {
			scanline += 1;
			width -= 1;
			rshift = 0;
		}
	}

	return( 0 );
}

/* Read a single encoded scanline.
 */
static int
scanline_read( Buffer *buffer, COLR *scanline, int width )
{
	int i, j;

	g_assert( buffer->length >= 0 ); 
	g_assert( buffer->position >= 0 ); 
	g_assert( buffer->position <= buffer->length ); 

	/* Detect old-style scanlines.
	 */
	if( width < MINELEN ||
		width > MAXELEN )
		return( scanline_read_old( buffer, scanline, width ) );

	if( buffer_need( buffer, 4 ) )
		return( -1 ); 

	if( BUFFER_PEEK( buffer ) != 2 ) 
		return( scanline_read_old( buffer, scanline, width ) );

	scanline[0][RED] = BUFFER_FETCH( buffer );
	scanline[0][GRN] = BUFFER_FETCH( buffer );
	scanline[0][BLU] = BUFFER_FETCH( buffer );
	scanline[0][EXP] = BUFFER_FETCH( buffer );
	if( scanline[0][GRN] != 2 || 
		scanline[0][BLU] & 128 ) 
		return( scanline_read_old( buffer, scanline + 1, width - 1 ) );

	if( ((scanline[0][BLU] << 8) | scanline[0][EXP]) != width ) {
		vips_error( "rad2vips", "%s", _( "scanline length mismatch" ) );
		return( -1 ); 
	}

	for( i = 0; i < 4; i++ ) 
		for( j = 0; j < width; ) {
			int code, len;
			gboolean run;

			if( buffer_need( buffer, 2 ) )
				return( -1 ); 

			code = BUFFER_FETCH( buffer ); 
			run = code > 128;
			len = run ? code & 127 : code; 

			if( j + len > width ) {
				vips_error( "rad2vips", "%s", _( "overrun" ) ); 
				return( -1 );
			}

			if( run ) { 
				int val;

				val = BUFFER_FETCH( buffer ); 
				while( len-- )
					scanline[j++][i] = val;
			} 
			else {
				if( buffer_need( buffer, len ) )
					return( -1 ); 
				while( len-- ) 
					scanline[j++][i] = 
						BUFFER_FETCH( buffer );
			}
		}

	return( 0 );
}

/* An encoded scanline can't be larger than this.
 */
#define MAX_LINE (2 * MAXELEN * sizeof( COLR ))

/* write an RLE scanline. Write magic header.
 */
static void
rle_scanline_write( COLR *scanline, int width, 
	unsigned char *buffer, int *length )
{
	int i, j, beg, cnt;

#define PUTC( CH ) { \
	buffer[(*length)++] = (CH); \
	g_assert( *length <= MAX_LINE ); \
}

	*length = 0;

	PUTC( 2 ); 
	PUTC( 2 ); 
	PUTC( width >> 8 ); 
	PUTC( width & 255 ); 

	for( i = 0; i < 4; i++ ) {
		for( j = 0; j < width; ) {
			/* Not needed, but keeps gcc used-before-set warning
			 * quiet.
			 */
			cnt = 1;

			/* Set beg / cnt to the start and length of the next 
			 * run longer than MINRUN.
			 */
			for( beg = j; beg < width; beg += cnt ) {
				for( cnt = 1; 
					cnt < 127 && 
					beg + cnt < width &&
					scanline[beg + cnt][i] == 
						scanline[beg][i]; 
					cnt++ )
					;

				if( cnt >= MINRUN )
					break;
			}

			/* Code pixels leading up to the run as a set of
			 * non-runs. 
			 */
			while( j < beg ) {
				int len = VIPS_MIN( 128, beg - j ); 
				COLR *p = scanline + j; 

				int k;

				PUTC( len ); 
				for( k = 0; k < len; k++ )
					PUTC( p[k][i] );
				j += len;
			}

			/* Code the run we found, if any
			 */
			if( cnt >= MINRUN ) {
				PUTC( 128 + cnt ); 
				PUTC( scanline[j][i] ); 
				j += cnt; 
			} 
		}
	}
}

/* Write a single scanline. buffer is at least MAX_LINE bytes and is used to
 * construct the RLE scanline. Don't allocate this on the stack so we don't
 * die too horribly on small-stack libc.
 */
static int
scanline_write( unsigned char *buffer, COLR *scanline, int width, FILE *fp )
{
	if( width < MINELEN || 
		width > MAXELEN )
		/* Write as a flat scanline.
		 */
		return( fwrite( scanline, sizeof( COLR ), width, fp ) - width );
	else {
		/* An RLE scanline.
		 */
		int length;

		rle_scanline_write( scanline, width, buffer, &length );

		g_assert( length <= MAX_LINE ); 

		return( fwrite( buffer, 1, length, fp ) - length );
	}
}

/* What we track during radiance file read.
 */
typedef struct {
	char *filename;
	VipsImage *out;

	FILE *fin;
	char format[256];
	double expos;
	COLOR colcor;
	double aspect;
	RGBPRIMS prims;
	RESOLU rs;
	Buffer *buffer; 
} Read;

int
vips__rad_israd( const char *filename )
{
	FILE *fin;
	char format[256];
	int result;

#ifdef DEBUG
	printf( "israd: \"%s\"\n", filename );
#endif /*DEBUG*/

        if( !(fin = vips__file_open_read( filename, NULL, FALSE )) ) 
		return( 0 );
	strcpy( format, PICFMT );
	result = checkheader( fin, format, NULL );
	fclose( fin );

	return( result == 1 );
}

static void
read_destroy( VipsObject *object, Read *read )
{
	VIPS_FREEF( buffer_free, read->buffer );
	VIPS_FREEF( fclose, read->fin );
	VIPS_FREE( read->filename );
}

static Read *
read_new( const char *filename, VipsImage *out )
{
	Read *read;
	int i;

	if( !(read = VIPS_NEW( out, Read )) )
		return( NULL );

	read->filename = vips_strdup( NULL, filename );
	read->out = out;
	read->fin = NULL;
	strcpy( read->format, COLRFMT );
	read->expos = 1.0;
	for( i = 0; i < 3; i++ )
		read->colcor[i] = 1.0;
	read->aspect = 1.0;
	read->prims[0][0] = CIE_x_r;
	read->prims[0][1] = CIE_y_r;
	read->prims[1][0] = CIE_x_g;
	read->prims[1][1] = CIE_y_g;
	read->prims[2][0] = CIE_x_b;
	read->prims[2][1] = CIE_y_b;
	read->prims[3][0] = CIE_x_w;
	read->prims[3][1] = CIE_y_w;
	read->buffer = NULL;

	g_signal_connect( out, "close", 
		G_CALLBACK( read_destroy ), read );

	if( !(read->fin = vips__file_open_read( filename, NULL, FALSE )) || 
		!(read->buffer = buffer_new( read->fin )) )
		return( NULL );

	return( read );
}

static int
rad2vips_process_line( char *line, Read *read )
{
	if( isformat( line ) ) {
		if( formatval( line, read->format ) )
			return( -1 );
	}
	else if( isexpos( line ) ) {
		read->expos *= exposval( line );
	}
	else if( iscolcor( line ) ) {
		COLOR cc;
		int i;

		(void) colcorval( cc, line );
		for( i = 0; i < 3; i++ )
			read->colcor[i] *= cc[i];
	}
	else if( isaspect( line ) ) {
		read->aspect *= aspectval( line );
	}
	else if( isprims( line ) ) {
		(void) primsval( read->prims, line );
	}

	return( 0 );
}

static const char *prims_name[4][2] = {
	{ "rad-prims-rx", "rad-prims-ry" }, 
	{ "rad-prims-gx", "rad-prims-gy" },
	{ "rad-prims-bx", "rad-prims-by" },
	{ "rad-prims-wx", "rad-prims-wy" }
};

static const char *colcor_name[3] = {
	"rad-colcor-r",
	"rad-colcor-g",
	"rad-colcor-b"
};

static int
rad2vips_get_header( Read *read, VipsImage *out )
{
	VipsInterpretation interpretation;
	int width;
	int height;
	int i, j;

	if( getheader( read->fin, (gethfunc *) rad2vips_process_line, read ) ||
		!fgetsresolu( &read->rs, read->fin ) ) {
		vips_error( "rad2vips", "%s", 
			_( "error reading radiance header" ) );
		return( -1 );
	}

	if( strcmp( read->format, COLRFMT ) == 0 )
		interpretation = VIPS_INTERPRETATION_scRGB;
	else if( strcmp( read->format, CIEFMT ) == 0 )
		interpretation = VIPS_INTERPRETATION_XYZ;
	else
		interpretation = VIPS_INTERPRETATION_MULTIBAND;

	width = scanlen( &read->rs );
	height = numscans( &read->rs );
	if( width <= 0 || 
		width > VIPS_MAX_COORD ||
		height <= 0 || 
		height > VIPS_MAX_COORD ) {
		vips_error( "rad2vips", "%s", _( "image size out of bounds" ) );
		return( -1 );
	}

	vips_image_init_fields( out, width, height, 4,
		VIPS_FORMAT_UCHAR, VIPS_CODING_RAD,
		interpretation,
		1, read->aspect );

	vips_image_pipelinev( out, VIPS_DEMAND_STYLE_THINSTRIP, NULL );

	vips_image_set_string( out, "rad-format", read->format );

	vips_image_set_double( out, "rad-expos", read->expos );

	for( i = 0; i < 3; i++ )
		vips_image_set_double( out, 
			colcor_name[i], read->colcor[i] );

	vips_image_set_double( out, "rad-aspect", read->aspect );

	for( i = 0; i < 4; i++ )
		for( j = 0; j < 2; j++ )
			vips_image_set_double( out, 
				prims_name[i][j], read->prims[i][j] );

	return( 0 );
}

int
vips__rad_header( const char *filename, VipsImage *out )
{
	Read *read;

#ifdef DEBUG
	printf( "rad2vips_header: reading \"%s\"\n", filename );
#endif /*DEBUG*/

	if( !(read = read_new( filename, out )) ) 
		return( -1 );
	if( rad2vips_get_header( read, read->out ) ) 
		return( -1 );

	return( 0 );
}

static int
rad2vips_generate( VipsRegion *or, 
	void *seq, void *a, void *b, gboolean *stop )
{
        VipsRect *r = &or->valid;
	Read *read = (Read *) a; 

	int y;

#ifdef DEBUG
	printf( "rad2vips_generate: line %d, %d rows\n", 
		r->top, r->height );
#endif /*DEBUG*/

	VIPS_GATE_START( "rad2vips_generate: work" );

	for( y = 0; y < r->height; y++ ) {
		COLR *buf = (COLR *) 
			VIPS_REGION_ADDR( or, 0, r->top + y );

		if( scanline_read( read->buffer, buf, or->im->Xsize ) ) {
			vips_error( "rad2vips", 
				_( "read error line %d" ), r->top + y );
			VIPS_GATE_STOP( "rad2vips_generate: work" );
			return( -1 );
		}
	}

	VIPS_GATE_STOP( "rad2vips_generate: work" );

	return( 0 );
}

int
vips__rad_load( const char *filename, VipsImage *out )
{
	VipsImage **t = (VipsImage **) 
		vips_object_local_array( VIPS_OBJECT( out ), 3 );

	Read *read;

#ifdef DEBUG
	printf( "rad2vips: reading \"%s\"\n", filename );
#endif /*DEBUG*/

	if( !(read = read_new( filename, out )) ) 
		return( -1 );

	t[0] = vips_image_new();
	if( rad2vips_get_header( read, t[0] ) )
		return( -1 );

	if( vips_image_generate( t[0], 
		NULL, rad2vips_generate, NULL, read, NULL ) ||
		vips_sequential( t[0], &t[1], 
			"tile_height", VIPS__FATSTRIP_HEIGHT, 
			NULL ) ||
		vips_image_write( t[1], out ) )
		return( -1 );

	return( 0 );
}

/* What we track during a radiance write.
 */
typedef struct {
	VipsImage *in;

	char *filename;
	FILE *fout;

	VipsDbuf dbuf; 

	char format[256];
	double expos;
	COLOR colcor;
	double aspect;
	RGBPRIMS prims;
	RESOLU rs;
} Write;

static void
write_destroy( Write *write )
{
	VIPS_FREE( write->filename );
	VIPS_FREEF( fclose, write->fout );
	vips_dbuf_destroy( &write->dbuf );

	vips_free( write );
}

static Write *
write_new( VipsImage *in )
{
	Write *write;
	int i;

	if( !(write = VIPS_NEW( NULL, Write )) )
		return( NULL );

	write->in = in;

	write->filename = NULL;
	write->fout = NULL;

	vips_dbuf_init( &write->dbuf ); 

	strcpy( write->format, COLRFMT );
	write->expos = 1.0;
	for( i = 0; i < 3; i++ )
		write->colcor[i] = 1.0;
	write->aspect = 1.0;
	write->prims[0][0] = CIE_x_r;
	write->prims[0][1] = CIE_y_r;
	write->prims[1][0] = CIE_x_g;
	write->prims[1][1] = CIE_y_g;
	write->prims[2][0] = CIE_x_b;
	write->prims[2][1] = CIE_y_b;
	write->prims[3][0] = CIE_x_w;
	write->prims[3][1] = CIE_y_w;

	return( write );
}

static void
vips2rad_make_header( Write *write )
{
	const char *str;
	int i, j;
	double d;

	if( vips_image_get_typeof( write->in, "rad-expos" ) )
		vips_image_get_double( write->in, "rad-expos", &write->expos );

	if( vips_image_get_typeof( write->in, "rad-aspect" ) )
		vips_image_get_double( write->in, "rad-aspect", &write->aspect );

	if( vips_image_get_typeof( write->in, "rad-format" ) &&
		!vips_image_get_string( write->in, "rad-format", &str ) )
		vips_strncpy( write->format, str, 256 );

	if( write->in->Type == VIPS_INTERPRETATION_scRGB )
		strcpy( write->format, COLRFMT );
	if( write->in->Type == VIPS_INTERPRETATION_XYZ )
		strcpy( write->format, CIEFMT );

	for( i = 0; i < 3; i++ )
		if( vips_image_get_typeof( write->in, colcor_name[i] ) && 
			!vips_image_get_double( write->in, colcor_name[i], &d ) )
			write->colcor[i] = d;

	for( i = 0; i < 4; i++ )
		for( j = 0; j < 2; j++ ) {
			const char *name = prims_name[i][j]; 

			if( vips_image_get_typeof( write->in, name ) &&
				!vips_image_get_double( write->in, name, &d ) )
				write->prims[i][j] = d;
		}

	/* Make y decreasing for consistency with vips.
	 */
	write->rs.rt = YDECR | YMAJOR;
	write->rs.xr = write->in->Xsize;
	write->rs.yr = write->in->Ysize;
}

static int
vips2rad_put_header( Write *write )
{
	vips2rad_make_header( write );

	fprintf( write->fout, "#?RADIANCE\n" );

	fputformat( write->format, write->fout );
	fputexpos( write->expos, write->fout );
	fputcolcor( write->colcor, write->fout );
	fprintf( write->fout, "SOFTWARE=vips %s\n", vips_version_string() );
	fputaspect( write->aspect, write->fout );
	fputprims( write->prims, write->fout );
	fputs( "\n", write->fout );
	fputsresolu( &write->rs, write->fout );

	return( 0 );
}

static int
vips2rad_put_data_block( VipsRegion *region, VipsRect *area, void *a )
{
	Write *write = (Write *) a;

	size_t size;
	unsigned char *buffer;
	int i;

	/* You have to seek back after a write.
	 */
	buffer = vips_dbuf_get_write( &write->dbuf, &size );
	vips_dbuf_seek( &write->dbuf, 0, SEEK_SET ); 

	g_assert( size >= MAX_LINE ); 

	for( i = 0; i < area->height; i++ ) {
		VipsPel *p = VIPS_REGION_ADDR( region, 0, area->top + i );

		if( scanline_write( buffer, 
			(COLR *) p, area->width, write->fout ) ) 
			return( -1 );
	}

	return( 0 );
}

static int
vips2rad_put_data( Write *write )
{
	if( vips_sink_disc( write->in, vips2rad_put_data_block, write ) )
		return( -1 );

	return( 0 );
}

int
vips__rad_save( VipsImage *in, const char *filename )
{
	Write *write;

#ifdef DEBUG
	printf( "vips2rad: writing \"%s\"\n", filename );
#endif /*DEBUG*/

	if( vips_image_pio_input( in ) ||
		vips_check_coding_rad( "vips2rad", in ) )
		return( -1 );
	if( !(write = write_new( in )) )
		return( -1 );

	write->filename = vips_strdup( NULL, filename );
	write->fout = vips__file_open_write( filename, FALSE );

	/* scanline_write() needs a buffer to write compressed scanlines to.
	 * We use the dbuf ... why not.
	 */
	vips_dbuf_allocate( &write->dbuf, MAX_LINE );

	if( !write->filename || 
		!write->fout ||
		vips2rad_put_header( write ) ||
		vips2rad_put_data( write ) ) {
		write_destroy( write );
		return( -1 );
	}
	write_destroy( write );

	return( 0 );
}

static int
vips2rad_put_header_buf( Write *write )
{
	vips2rad_make_header( write );

	vips_dbuf_writef( &write->dbuf, "#?RADIANCE\n" );
	vips_dbuf_writef( &write->dbuf, "%s%s\n", FMTSTR, write->format );
	vips_dbuf_writef( &write->dbuf, "%s%e\n", EXPOSSTR, write->expos );
	vips_dbuf_writef( &write->dbuf, "%s %f %f %f\n", 
		COLCORSTR, 
		write->colcor[RED], write->colcor[GRN], write->colcor[BLU] );
	vips_dbuf_writef( &write->dbuf, "SOFTWARE=vips %s\n", 
		vips_version_string() );
	vips_dbuf_writef( &write->dbuf, "%s%f\n", ASPECTSTR, write->aspect );
	vips_dbuf_writef( &write->dbuf, 
		"%s %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n", 
		PRIMARYSTR, 
		write->prims[RED][CIEX], write->prims[RED][CIEY], 
		write->prims[GRN][CIEX], write->prims[GRN][CIEY], 
		write->prims[BLU][CIEX], write->prims[BLU][CIEY], 
		write->prims[WHT][CIEX], write->prims[WHT][CIEY] );
	vips_dbuf_writef( &write->dbuf, "\n" );
	vips_dbuf_writef( &write->dbuf, "%s", 
		resolu2str( resolu_buf, &write->rs ) );

	return( 0 );
}

/* Write a single scanline to buffer.
 */
static int
scanline_write_buf( Write *write, COLR *scanline, int width )
{
	unsigned char *buffer;
	size_t size;
	int length;

	vips_dbuf_allocate( &write->dbuf, MAX_LINE );
	buffer = vips_dbuf_get_write( &write->dbuf, &size );

	if( width < MINELEN || 
		width > MAXELEN ) {
		/* Write as a flat scanline.
		 */
		length = sizeof( COLR ) * width;
		memcpy( buffer, scanline, length ); 
	}
	else 
		/* An RLE scanline.
		 */
		rle_scanline_write( scanline, width, buffer, &length );

	vips_dbuf_seek( &write->dbuf, length - size, SEEK_CUR ); 

	return( 0 );
}

static int
vips2rad_put_data_block_buf( VipsRegion *region, VipsRect *area, void *a )
{
	Write *write = (Write *) a;
	int i;

	for( i = 0; i < area->height; i++ ) {
		VipsPel *p = VIPS_REGION_ADDR( region, 0, area->top + i );

		if( scanline_write_buf( write, (COLR *) p, area->width ) ) 
			return( -1 );
	}

	return( 0 );
}

static int
vips2rad_put_data_buf( Write *write )
{
	if( vips_sink_disc( write->in, vips2rad_put_data_block_buf, write ) )
		return( -1 );

	return( 0 );
}

int
vips__rad_save_buf( VipsImage *in, void **obuf, size_t *olen )
{
	Write *write;

#ifdef DEBUG
	printf( "vips2rad: writing to buffer\n" );
#endif /*DEBUG*/

	if( vips_image_pio_input( in ) ||
		vips_check_coding_rad( "vips2rad", in ) )
		return( -1 );
	if( !(write = write_new( in )) ) 
		return( -1 );

	if( vips2rad_put_header_buf( write ) ||
		vips2rad_put_data_buf( write ) ) {
		write_destroy( write );
		return( -1 );
	}

	*obuf = vips_dbuf_steal( &write->dbuf, olen );

	write_destroy( write );

	return( 0 );
}

const char *vips__rad_suffs[] = { ".hdr", NULL };

#endif /*HAVE_RADIANCE*/
