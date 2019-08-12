#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <keyboard.h>
#include <mouse.h>

Image *canvas;
Image *in;
Image *out; /* add a write queue */
Image *clear;

QLock ql;

#define DEBUG(s) fprint(2, s "\n");

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
	lockdisplay(display);
	unloadimage(i, i->r, buf+5*12, s);
	unlockdisplay(display);
	n = pwrite(fd, buf, 5*12+s, 0);
	if(n < 5*12+s)
		fprint(2, "write failed, fr*ck\n");
}

Image *
getcanvasimage(int fd)
{
	Image *i;
	
	qlock(&ql);
	seek(fd, 0, 0);
	i = readimage(display, fd, 1);
	qunlock(&ql);
	if(i == nil)
		sysfatal("get: %r");
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
	lockdisplay(display);
	draw(screen, screen->r, display->black, nil, ZP);
	unlockdisplay(display);
}

void
redraw(void)
{
	lockdisplay(display);
	draw(screen, screen->r, canvas, nil, ZP);
	draw(screen, screen->r, out, nil, ZP);
	unlockdisplay(display);
}

struct ups{
	int ufd;
	int cfd;
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
		in = getcanvasimage(ups.cfd);
		if(in == nil)
			sysfatal("Invalid image update read.");
		/* potential to add dynamic resizing */
		if(!eqrect(in->r, canvas->r))
			sysfatal("Canvas rectangle changed.");
		lockdisplay(display);
		draw(canvas, canvas->r, in, nil, ZP);
		freeimage(in);
		unlockdisplay(display);
		send(ups.chan, &i);
	}
}

void
threadmain(int argc, char **argv)
{
	Keyboardctl *kctl;
	Rune r;
	Mousectl *mctl;
	Mouse m, mold; /* don't you love puns? */
	int state;
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
	if((ups.cfd = open(path, ORDWR)) < 0)
		sysfatal("%r");
	snprint(path, sizeof(path), "%s/%s", dir, "update");
	if((ups.ufd = open(path, ORDWR)) < 0)
		sysfatal("%r");
	ups.chan = chancreate(sizeof(char), 1);
	if(initdraw(nil, nil, argv0) < 0)
		sysfatal("%r");
	display->locking = 1;
	unlockdisplay(display);
	if((mctl = initmouse(nil, screen)) == nil)
		sysfatal("%r");
	if((kctl = initkeyboard(nil)) == nil)
		sysfatal("%r");
	
	dogetwindow();
	
	n = pread(ups.cfd, hdr, 5*12, 0);
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
	
	canvas = getcanvasimage(ups.cfd);
	
	lockdisplay(display);
	out = allocimage(display, canvas->r, RGBA32, 0, DTransparent);
	clear = allocimage(display, Rect(0,0,1,1), RGBA32, 1, DTransparent);
	if(out == nil || clear == nil)
		sysfatal("Allocimage failed. %r");
	unlockdisplay(display);
	
	
	if(proccreate(updateproc, &ups, 1024) < 0)
		sysfatal("%r");
	
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
		lockdisplay(display);
		flushimage(display, 1);
		unlockdisplay(display);
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
				lockdisplay(display);
				line(out, subpt(mold.xy,screen->r.min), subpt(m.xy,screen->r.min),
					0, 0, 0, display->black, ZP);
				unlockdisplay(display);
				if(m.buttons != 1){
					state = 0;
					sendimage(ups.cfd, out);
					lockdisplay(display);
					drawop(out, out->r, clear, nil, ZP, S);
					unlockdisplay(display);
				}
				break;
			case 2:
				lockdisplay(display);
				line(out, subpt(mold.xy,screen->r.min), subpt(m.xy,screen->r.min),
					0, 0, 0, display->white, ZP);
				unlockdisplay(display);
				if(m.buttons != 4){
					state = 0;
					sendimage(ups.cfd, out);
					lockdisplay(display);
					drawop(out, out->r, clear, nil, ZP, S);
					unlockdisplay(display);
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
			redraw();
			break;
		case NONE:
			print("I'm a woodchuck, not a woodchucker! (thanks for playing)\n");
			goto noflush;
		}
	}
}
