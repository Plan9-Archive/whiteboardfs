#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <keyboard.h>
#include <mouse.h>

Image *canvas;
Image *in;
Image *out;
Image *clear;

void
sendimage(int fd, Image *i)
{
	char chanstr[12];
	uchar *buf;
	ulong s;
	long n;
	
	s = Dy(i->r)*bytesperline(i->r, i->depth);
	buf = malloc(5*12 + s);
	if(buf == nil)
		sysfatal("%r");
	snprint((char*)buf, 5*12+1, "%11s %11d %11d %11d %11d ", chantostr(chanstr, i->chan),
			i->r.min.x, i->r.min.y, i->r.max.x, i->r.max.y);
	unloadimage(i, i->r, buf+5*12, s);
	n = write(fd, buf, 5*12+s);
	if(n < 5*12+s)
		fprint(2, "write failed, fr*ck\n");
}

Image *
getcanvasimage(int fd)
{
	Image *i;
	seek(fd, 0, 0);
	i = readimage(display, fd, 0);
	if(i == nil)
		sysfatal("%r");
	if(i->r.min.x != 0 || i->r.min.y != 0 || i->r.max.x <= 0 || i->r.max.y <= 0)
		sysfatal("Bad image size.");
	if(i->r.max.x > 4096 || i->r.max.y > 2160)
		sysfatal("Image too large to be useful on most screens.");
	return i;
}

void
usage(void)
{
	fprint(2, "usage: %s [-d dir]\n", argv0);
	threadexitsall("usage");
}

void
dogetwindow(void)
{
	if(getwindow(display, Refnone) < 0)
		sysfatal("Cannot reconnect to display: %r");
	draw(screen, screen->r, display->black, nil, ZP);
}

void
redraw(void)
{
	draw(screen, screen->r, canvas, nil, ZP);
	draw(screen, screen->r, out, nil, ZP);
}

int
mousesub(Mouse m, int *state)
{
	static Mouse old;
	int redraw;
	
	redraw = 0;
	switch(*state){
	case 0:
		if(m.buttons == 1){
			*state = 1;
		}else if(m.buttons == 4){
			*state = 2;
		}
		break;
	case 1:
		if(m.buttons == 0){
		}else{
			line(out, old.xy, m.xy, 0, 0, 0, display->black, ZP);
		}
		redraw = 1;
		break;
	case 2:
		redraw = 1;
		break;
	}
	old = m;
	return redraw;
}

struct ups{
	int ufd;
	Channel *chan; /* char */
} ups;

void
updateproc(void*)
{
	/* uses struct ups instead of aux arg */
	char i;
	long n;
	
	for(;;){
		n = read(ups.ufd, &i, sizeof(i));
		if(n != 1){
			yield(); /* if error is due to exiting, we'll exit here */
			if(n < 0){
				fprint(2, "Whiteboard probably closed.\n");
				threadexitsall("read error");
			}
			fprint(2, "Why was there a size 0 read?\n");
		}
		send(ups.chan, &i);
	}
}

#define DEBUG() fprint(2, "LOL\n");

void
threadmain(int argc, char **argv)
{
	Keyboardctl *kctl;
	Rune r;
	Mousectl *mctl;
	Mouse m, mold; /* don't you love puns? */
	int cfd, state;
	char hdr[5*12+1];
	long n;
	char *dir, path[128];
	Rectangle rect;
	
	dir = "/mnt/whiteboard";
	ARGBEGIN{
	case 'd':
		dir = argv[0];
		break;
	default:
		usage();
	}ARGEND;
	
	snprint(path, sizeof(path), "%s/%s", dir, "canvas");
	if((cfd = open(path, ORDWR)) < 0)
		sysfatal("%r");
	snprint(path, sizeof(path), "%s/%s", dir, "update");
	if((ups.ufd = open(path, ORDWR)) < 0)
		sysfatal("%r");
	ups.chan = chancreate(sizeof(char), 1);
	if(initdraw(nil, nil, argv0) < 0)
		sysfatal("%r");
	if((mctl = initmouse(nil, screen)) == nil)
		sysfatal("%r");
	if((kctl = initkeyboard(nil)) == nil)
		sysfatal("%r");
	if(proccreate(updateproc, &ups, 1024) < 0)
		sysfatal("%r");
	
	dogetwindow();
	
	n = pread(cfd, hdr, 5*12, 0);
	if(n < 5*12){
		sysfatal("No image header.");
	}
	/* basic protection */
	hdr[11] = hdr[23] = hdr[35] = hdr[47] = hdr[59] = '\0';
	if(strtochan(hdr) == 0)
		sysfatal("Bad image channel.");
	rect.min.x = atoi(hdr+1*12);
	rect.min.y = atoi(hdr+2*12);
	rect.max.x = atoi(hdr+3*12);
	rect.max.y = atoi(hdr+4*12);
	if(rect.min.x != 0 || rect.min.y != 0 || rect.max.x <= 0 || rect.max.y <= 0)
		sysfatal("Bad image size.");
	if(rect.max.x > 4096 || rect.max.y > 2160)
		sysfatal("Image too large to be useful on most screens.");
	/* can be subverted for a massive size, despite previous checking */
	canvas = getcanvasimage(cfd);
	out = allocimage(display, canvas->r, RGBA32, 0, DTransparent);
	clear = allocimage(display, Rect(0,0,1,1), RGBA32, 1, DTransparent);
	if(out == nil || clear == nil)
		sysfatal("Allocimage failed. %r");
	
	state = 0;
	mold.buttons = 0;
	mold.xy = screen->r.min; /* arbitrary */
	
	dogetwindow();
	redraw();
	
	enum { MOUSE, RESIZE, KEYS, UPDATE, NONE };
	Alt alts[] = {
		[MOUSE] =  {mctl->c, &m, CHANRCV},
		[RESIZE] =  {mctl->resizec, nil, CHANRCV},
		[KEYS] = {kctl->c, &r, CHANRCV},
		[UPDATE] = {ups.chan, nil, CHANRCV},
		[NONE] =  {nil, nil, CHANEND}
	};
	
	for(;;){
		flushimage(display, 1);
noflush:
		switch(alt(alts)){
		case MOUSE:
			switch(state){
			case 0:
				if(m.buttons == 1)
					state = 1;
				else if(m.buttons == 4)
					state = 2;
				else
					goto noflush;
				break;
			case 1:
				line(out, subpt(mold.xy,screen->r.min), subpt(m.xy,screen->r.min),
					0, 0, 0, display->black, ZP);
				if(m.buttons != 1){
					state = 0;
					sendimage(cfd, out);
					drawop(out, out->r, clear, nil, ZP, S);
				}
				break;
			case 2:
				line(out, subpt(mold.xy,screen->r.min), subpt(m.xy,screen->r.min),
					0, 0, 0, display->white, ZP);
				if(m.buttons != 4){
					state = 0;
					sendimage(cfd, out);
					drawop(out, out->r, clear, nil, ZP, S);
				}
				break;
			}
			redraw();
			mold = m;
			break;
		case RESIZE:
			dogetwindow();
			redraw();
			break;
		case KEYS:
			if(r == Kdel)
				threadexitsall(nil);
			goto noflush;
		case UPDATE:
			in = getcanvasimage(cfd);
			if(in == nil)
				sysfatal("Invalid image update read.");
			/* potential to add dynamic resizing */
			if(!eqrect(in->r, canvas->r))
				sysfatal("Canvas rectangle changed.");
			draw(canvas, canvas->r, in, nil, ZP);
			redraw();
			break;
		case NONE:
			print("I'm a woodchuck, not a woodchucker! (thanks for playing)\n");
			goto noflush;
		}
	}
}
