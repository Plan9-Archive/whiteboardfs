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
Image *pencol;
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
	Point ss, lu, mid;
	int y;
	char *mesg;
	
	lockdisplay(display);
	draw(screen, screen->r, display->black, nil, ZP);
	mid.x = Dx(screen->r)/2 + screen->r.min.x;
	mid.y = Dy(screen->r)/2 + screen->r.min.y;
	draw(screen, Rect(mid.x, screen->r.min.y, screen->r.max.x, mid.y), display->white, nil, ZP);
	draw(screen, Rect(screen->r.min.x, mid.y, mid.x, screen->r.max.y), display->white, nil, ZP);
	draw(screen, screen->r, pencol, nil, ZP);
	drawpt.x = (Dx(screen->r) - Dx(canvas->r))/2 + screen->r.min.x;
	drawpt.y = (Dy(screen->r) - Dy(canvas->r))/2 + screen->r.min.y;
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

struct{
	Channel *chan;
	int fd;
} makeu;
void
makeuproc(void*){
	char buf[12];
	ulong col;
	int n;
	
	for(;;){
		n = read(makeu.fd, buf, sizeof(buf));
		if(n < 1){
			yield(); /* if error is due to exiting, we'll exit here */
			if(n < 0){
				fprint(2, "whiteboard probably closed.\n");
				threadexitsall("read error");
			}
			if(n == 0){
				fprint(2, "makeu closed?\n");
				//threadexitsall(nil);
			}
		}
		buf[sizeof(buf)-1] = '\0';
		col = strtoul(buf, nil, 0);
		send(makeu.chan, &col);
	}
}

void
threadmain(int argc, char **argv)
{
	static Keyboardctl *kctl;
	Rune r;
	static Mousectl *mctl;
	static Mouse m, mold; /* don't you love puns? */
	int state;
	static char hdr[5*12+1];
	long n;
	char *dir;
	static char path[128];
	Rectangle rect;
	ulong col;
	int cflag;
	uchar ldcol[4];
	int pid;
	int pfd[2];
	char *srvwsys;
	int wsysfd;
	
	dir = "/mnt/whiteboard";
	cflag = 0;
	ARGBEGIN{
	case 'd':
		dir = argv[0];
		break;
	case 'c':
		cflag = 1;
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
	
	if(cflag){
		if(pipe(pfd) < 0)
			sysfatal("%s: pipe failed: %r", argv0);
		makeu.fd = pfd[0];
		pid = rfork(RFFDG|RFREND|RFPROC|RFNOWAIT);
		if(pid == 0){
			dup(pfd[1], 1);
			srvwsys = getenv("wsys");
			if(srvwsys == nil)
				sysfatal("can't find $wsys: %r");
			rfork(RFNAMEG);
			wsysfd = open(srvwsys, ORDWR);
			if(wsysfd < 0)
				sysfatal("can't open $wsys: %r");
			if(mount(wsysfd, -1, "/mnt/wsys", MREPL, "new -dx 400 -dy 300") < 0)
				sysfatal("can't mount new window: %r");
			if(bind("/mnt/wsys", "/dev", MBEFORE) < 0)
				sysfatal("can't bind: %r");
			execl("/bin/makeu", "makeu", "-p", "-c", "0x000000FF", nil);
			fprint(2, "%s: makeu exec failed: %r", argv0);
			threadexitsall("makeu exec failed");
		}
		makeu.chan = chancreate(sizeof(ulong), 1);
		if(proccreate(makeuproc, &sends, 4096) < 0)
			sysfatal("%r");
	}else{
		makeu.chan = nil;
	}
	
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
	pencol = allocimage(display, Rect(0,0,1,1), RGBA32, 1, DBlack);
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
	enum { MOUSE, RESIZE, KEYS, UPDATE, MAKEU, NONE };
	Alt alts[] = {
		[MOUSE] =  {mctl->c, &m, CHANRCV},
		[RESIZE] =  {mctl->resizec, nil, CHANRCV},
		[KEYS] = {kctl->c, &r, CHANRCV},
		[UPDATE] = {ups.chan, nil, CHANRCV},
		[MAKEU] =  {makeu.chan, &col, CHANEND},
		[NONE] =  {nil, nil, CHANEND},
	};
	
	if(cflag)
		alts[MAKEU].op = CHANRCV;
	
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
					Enddisc, Enddisc, pensize, pencol, ZP);
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
		case MAKEU:
			ldcol[0] = col>>0 & 0xFF;
			ldcol[1] = col>>8 & 0xFF;
			ldcol[2] = col>>16 & 0xFF;
			ldcol[3] = col>>24 & 0xFF;
			loadimage(pencol, pencol->r, ldcol, sizeof(ldcol));
			redraw();
			break;
		case NONE:
			print("I'm a woodchuck, not a woodchucker! (thanks for playing)\n");
			goto noflush;
		}
	}
}
