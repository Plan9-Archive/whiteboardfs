/* Amavect! */
/*
 * This is a reimplementation of readimage, creadimage, writeimage, readmemimage,
 * creadmemimage, writememimage in such a way that breaks down into
 * functions that can be used to construct Images over 9p.
 * There is some repetition between the current readimage and creadimage, so I
 * thought I could consolidate that.
 *
 * neglects old image format
 * 
 * i like to put fake in front of my function defs.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <memdraw.h>

/* 
 * Allocate an empty memimage based on the 60 byte header.
 * Assumes *hdr contains points to at least 60 bytes.
 * returns nil on error
 */
Memimage *
hdrallocmemimage(char *hdr)
{
	ulong chan;
	Rectangle r;
	Memimage *i;
	
	if(hdr[11] != ' '){
		werrstr("hdrallocmemimage: bad format");
		return nil;
	}
	
	hdr[11] = '\0';
	if((chan = strtochan(hdr)) == 0){
		werrstr("hdrallocmemimage: bad channel string %s", hdr);
		return nil;
	}
	
	hdr[1*12+11] = hdr[2*12+11] = hdr[3*12+11] = hdr[4*12+11] = '\0';
	r.min.x=atoi(hdr+1*12);
	r.min.y=atoi(hdr+2*12);
	r.max.x=atoi(hdr+3*12);
	r.max.y=atoi(hdr+4*12);
	if(r.min.x > r.max.x || r.min.y > r.max.y){
		werrstr("hdrallocmemimage: bad rectangle");
		return nil;
	}

	i = allocmemimage(r, chan);
	if(i == nil)
		return nil;
	return i;
}

/*
 * send a maximum 6025 byte data buffer block
 * overwrites miny to be the the last row updated
 * returns number of bytes read from buffer, -1 on failure
 * 
 * currently returns -1 if n is not the same as the block size.
 */
int
blockcloadmemimage(Memimage *i, uchar *buf, int n, int *miny)
{
	int maxy;
	int nb, ncblock;
	
	if(n < 2*12){
		werrstr("blockcloadmemimage: short read");
		return -1;
	}
	buf[0*12+11] = buf[1*12+11] = '\0';
	nb = atoi((char*)buf+1*12);
	if(n < nb + 2*12)
		return 0;
	maxy = atoi((char*)buf+0*12);
	if(maxy <= *miny || maxy > i->r.max.y){
		werrstr("blockcloadmemimage: bad maxy %d", maxy);
		return -1;
	}
	ncblock = _compblocksize(i->r, i->depth);
	if(nb <= 0 || nb > ncblock){
		werrstr("blockcloadmemimage: bad count %d", nb);
		return -1;
	}
	if(nb + 2*12 != n){
		werrstr("blockcloadmemimage: block size != buffer size, %d != %d", nb, n);
		return -1;
	}
	if(cloadmemimage(i, Rect(i->r.min.x, *miny, i->r.max.x, maxy), buf+2*12, nb) < 0){
		werrstr("blockcloadmemimage: bad cloadmemimage");
		return -1;
	}
	*miny = maxy;
	return nb;
}

/* 
 * returns number of bytes written. 
 * returns -1 on error, or 0 on no bytes written
 */
int
lineloadmemimage(Memimage *i, uchar *buf, int n, int *miny)
{
	int l, dy;
	
	l = bytesperline(i->r, i->depth);
	if(l <= 0){
		werrstr("blockloadmemimage: bad bytes per line size %d", l);
		return -1;
	}
	dy = n / l;
	if(dy < 0){
		werrstr("blockloadmemimage: n was negative %d", n);
		return -1;
	}else if(dy == 0){
		return 0;
	}
	if(dy + *miny > i->r.max.y){
		werrstr("blockloadmemimage: buf size too large %d", n);
		return -1;
	}
	if(loadmemimage(i, Rect(i->r.min.x, *miny, i->r.max.x, *miny+dy), buf, dy*l) < 0){
		werrstr("blockloadmemimage: bad loadmemimage");
		return -1;
	}
	*miny += dy;
	return dy*l;
}

int
blockloadmemimage(Memimage *i, uchar *buf, int n, int *miny, int comp)
{
	if(comp)
		return blockcloadmemimage(i, buf, n, miny);
	else
		return lineloadmemimage(i, buf, n, miny);
}
