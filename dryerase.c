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
Image *grey;
Point drawpt;
int pensize;
int writing;
int reading;

QLock qout;
QLock ql;

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
	lockdisplay(display);
	if(getwindow(display, Refnone) < 0)
		sysfatal("Cannot reconnect to display: %r");
	unlockdisplay(display);
}

void
redraw(void)
{
	Point ss, lu;
	int y;
	char *mesg;
	
	lockdisplay(display);
	drawpt.x = (Dx(screen->r) - Dx(canvas->r))/2 + screen->r.min.x;
	drawpt.y = (Dy(screen->r) - Dy(canvas->r))/2 + screen->r.min.y;
	draw(screen, screen->r, display->black, nil, ZP);
	border(screen, rectaddpt(canvas->r, drawpt), -2, grey, ZP);
	draw(screen, screen->r, canvas, nil, subpt(screen->r.min, drawpt));
	draw(screen, screen->r, out, nil, subpt(screen->r.min, drawpt));
	if(writing){
		mesg = "writing...";
	}else{
		mesg = "ready.";
	}
	ss = stringsize(display->defaultfont, mesg);
	draw(screen, Rpt(screen->r.min, addpt(screen->r.min, ss)), display->white, nil, ZP);
	string(screen, screen->r.min, display->black, ZP, display->defaultfont, mesg);
	
	if(reading){
		mesg = "reading...";
	}else{
		mesg = "done.";
	}
	y = ss.y;
	ss = stringsize(display->defaultfont, mesg);
	ss.y += y;
	lu = addpt(screen->r.min, Pt(0,y));
	draw(screen, Rpt(lu, addpt(screen->r.min, ss)), display->white, nil, ZP);
	string(screen, lu, display->black, ZP, display->defaultfont, mesg);
	
	unlockdisplay(display);
}

struct{
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
	
	threadsetname("updater");
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
		reading = 1;
		nbsend(ups.chan, nil);
		in = getcanvasimage(ups.cfd);
		if(in == nil)
			sysfatal("Invalid image update read.");
		/* potential to add dynamic resizing */
		if(!eqrect(in->r, canvas->r))
			sysfatal("Canvas rectangle changed.");
		lockdisplay(display);
		draw(canvas, canvas->r, in, nil, ZP);
		freeimage(in);
		reading = 0;
		unlockdisplay(display);
		nbsend(ups.chan, &i);
	}
}

struct{
	int cfd;
	Channel *chan; /* char */
} sends;
void
sendproc(void*)
{
	/* uses struct ups instead of aux arg */
	int n;
	
	threadsetname("sender");
	for(;;){
		n = recv(sends.chan, nil);
		if(n != 1){
			yield(); /* if error is due to exiting, we'll exit here */
			if(n < 0){
				fprint(2, "Whiteboard probably closed.\n");
				threadexitsall("recv error");
			}
			fprint(2, "Why was there a size 0 send?\n");
		}
		
		qlock(&qout);
		writeimage(sends.cfd, out, 1);
		drawop(out, out->r, clear, nil, ZP, S);
		qunlock(&qout);
		writing = 0;
		nbsend(ups.chan, nil);
	}
}

void
sendoutimage(void)
{
	writing = 1;
	send(sends.chan, nil);
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
	
	snprint(path, sizeof(path), "%s/%s", dir, "canvas.bit");
	if((ups.cfd = open(path, ORDWR)) < 0)
		sysfatal("%r");
	/*
	 * readimage and writeimage both implicitly seek on the fd.
	 * dup doesn't separate the seeks, so open again.
	 */
	if((sends.cfd = open(path, ORDWR)) < 0)
		sysfatal("%r");
	snprint(path, sizeof(path), "%s/%s", dir, "update");
	if((ups.ufd = open(path, ORDWR)) < 0)
		sysfatal("%r");
	ups.chan = chancreate(sizeof(char), 1);
	sends.chan = chancreate(sizeof(char), 0);
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
	grey = allocimage(display, Rect(0,0,1,1), RGBA32, 1, 0x555555FF);
	if(out == nil || clear == nil || grey == nil)
		sysfatal("Allocimage failed. %r");
	unlockdisplay(display);
	
	if(proccreate(updateproc, &ups, 4096) < 0)
		sysfatal("%r");
	if(proccreate(sendproc, &sends, 4096) < 0)
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
				qlock(&qout);
				lockdisplay(display);
				line(out, subpt(mold.xy, drawpt), subpt(m.xy, drawpt),
					Enddisc, Enddisc, pensize, display->black, ZP);
				unlockdisplay(display);
				qunlock(&qout);
				if(m.buttons != 1){
					state = 0;
					qlock(&qout);
					lockdisplay(display);
					sendoutimage();
					draw(canvas, out->r, out, nil, ZP);
					unlockdisplay(display);
					qunlock(&qout);
				}
				break;
			case 2:
				qlock(&qout);
				lockdisplay(display);
				line(out, subpt(mold.xy, drawpt), subpt(m.xy, drawpt),
					Enddisc, Enddisc, pensize, display->white, ZP);
				unlockdisplay(display);
				qunlock(&qout);
				if(m.buttons != 4){
					state = 0;
					qlock(&qout);
					lockdisplay(display);
					sendoutimage();
					draw(canvas, out->r, out, nil, ZP);
					unlockdisplay(display);
					qunlock(&qout);
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
			if(r == Kdel){
				threadexitsall(nil);
			}else if(r == ']'){
				pensize += 1;
				if(pensize > 42)
					pensize = 42;
			}else if(r == '['){
				pensize -= 1;
				if(pensize < 0)
					pensize = 0;
			}
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
