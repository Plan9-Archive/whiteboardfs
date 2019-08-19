/* Amavect! */
/*
 * Test program to write images to /mnt/whiteboard/canvas
 * canvas must be of size 256x256
 */
#include <u.h>
#include <libc.h>

char hdr[5*12+1];
uchar buf[4*256*256];

void
main(int argc, char *argv[])
{
	int canvasfd, imtype, i, n;
	uchar r, g, b, a;
	
	imtype = 0;
	ARGBEGIN{
	case 'w':
		imtype = 0;
		break;
	case 'k':
		imtype = 1;
		break;
	case 'r':
		imtype = 2;
		break;
	case 'g':
		imtype = 3;
		break;
	case 'b':
		imtype = 4;
		break;
	default:
		break;
	}ARGEND;
	
	canvasfd = open("/mnt/whiteboard/canvas.bit", OWRITE);
	if(canvasfd < 0)
		sysfatal("%r");
	
	switch(imtype){
	default:
		r = 0xFF;
		g = 0xFF;
		b = 0xFF;
		a = 0xFF;
		break;
	case 1:
		r = 0x00;
		g = 0x00;
		b = 0x00;
		a = 0xFF;
		break;
	case 2:
		r = 0xFF;
		g = 0x00;
		b = 0x00;
		a = 0xFF;
		break;
	case 3:
		r = 0x00;
		g = 0xFF;
		b = 0x00;
		a = 0xFF;
		break;
	case 4:
		r = 0x00;
		g = 0x00;
		b = 0xFF;
		a = 0xFF;
		break;
	}
	snprint(hdr, sizeof(hdr), "   r8g8b8a8 %11d %11d %11d %11d ", 0, 0, 256, 256);
	for(i = 0; i < sizeof(buf); i += 4){
		buf[0+i] = a;
		buf[1+i] = b;
		buf[2+i] = g;
		buf[3+i] = r;
	}
	fprint(2, "%d\n", i);
	n = write(canvasfd, hdr, 5*12);
	fprint(2, "hdr %d\n", n);
	if(n < 60)
		sysfatal("Did not write 60 bytes: %d", n);
	n = write(canvasfd, buf, sizeof(buf));
	fprint(2, "buf %d\n", n);
	if(n < 4*256*256)
		sysfatal("Did not write 4*256*256 bytes: %r");
	exits(nil);
}
