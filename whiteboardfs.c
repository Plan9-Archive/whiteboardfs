/* Amavect! */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <draw.h>
#include <memdraw.h>

enum {
	Qroot,
	Qcanvas,
	Qupdate,
};

typedef struct F {
	char *name;
	Qid qid;
	ulong perm;
} F;

typedef struct Readq Readq;
struct Readq {
	Req *this;
	Readq *last; /* doubly linked list */
	Readq *link;
};

typedef struct Writeq Writeq;
struct Writeq {
	Rectangle r;
	ulong chan;
	uvlong n; /* buf size, must be met exactly else Rerror */
	uvlong i; /* current buffer index */
	uchar *buf; /* write buffer, size 5*12+bytesperline*rows */
};

typedef struct Updateq Updateq;
struct Updateq {
	Updateq *last; /* doubly linked list */
	Updateq *link;
	u32int fid;
	uchar hasread;
};

char *user;
Memimage *canvas, *diff;
char imhdr[5*12+1];
uchar *imdata;
Updateq *fqhead, *fqtail;
Readq *rqhead, *rqtail;

F root = {"/", {Qroot, 0, QTDIR}, 0555|DMDIR};
F canvasf = {"canvas", {Qcanvas, 0, QTFILE}, 0666};
F updatef = {"update", {Qupdate, 0, QTFILE}, 0666};

void
usage(void)
{
	fprint(2, "%s [-D] [-m mtpt] [-s srv] [-x width] [-y height] \n", argv0);
	exits("usage");
}

ulong
memimagebytelen(Memimage *i)
{
	return Dy(i->r)*bytesperline(i->r, i->depth);
}

void
imdataupdate(void)
{
	unloadmemimage(canvas, canvas->r, imdata, memimagebytelen(canvas));
}

/* update the imhdr, reallocate imdata, and update imdata */
void
imhdrupdate(void)
{
	char ch[11];
	snprint(imhdr, sizeof(imhdr), "%11s %11d %11d %11d %11d ",
		chantostr(ch, canvas->chan), canvas->r.min.x, 
		canvas->r.min.y, canvas->r.max.x, canvas->r.max.y);
	if(imdata != nil)
		imdata = realloc(imdata, memimagebytelen(canvas));
	else
		imdata = malloc(memimagebytelen(canvas));
	imdataupdate();
}

F *
filebypath(uvlong path)
{
	if(path == Qroot)
		return &root;
	else if(path == Qcanvas)
		return &canvasf;
	else if(path == Qupdate)
		return &updatef;
	else
		return nil;
}

void 
fsattach(Req *r)
{
	r->fid->qid = filebypath(Qroot)->qid;
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

char *
fswalk1(Fid *fid, char *name, Qid *qid)
{
	if(fid->qid.path == Qroot){
		if(strcmp(name, canvasf.name) == 0){
			*qid = canvasf.qid;
			fid->qid = *qid;
			return nil;
		}else if(strcmp(name, updatef.name) == 0){
			*qid = updatef.qid;
			fid->qid = *qid;
			return nil;
		}else if(strcmp(name, "..") == 0){
			*qid = root.qid;
			fid->qid = *qid;
			return nil;
		}	
	}
	return "not found";
}

void
fillstat(Dir *d, uvlong path)
{
	F *f;
	
	f = filebypath(path);
	d->qid = f->qid;	/* unique id from server */
	d->mode = f->perm;	/* permissions */
	d->atime = time(0);	/* last read time */
	d->mtime = time(0);	/* last write time */
	d->length = 0;	/* file length */
	d->name = estrdup9p(f->name);	/* last element of path */
	d->uid = estrdup9p(user);	/* owner name */
	d->gid = estrdup9p(user);	/* group name */
	d->muid = estrdup9p(user);	/* last modifier name */
}

void
fsstat(Req *r)
{
	fillstat(&r->d, r->fid->qid.path);
	respond(r, nil);
}

int
rootgen(int n, Dir *d, void *)
{
	switch(n){
	case 0:
		fillstat(d, canvasf.qid.path);
		break;
	case 1:
		fillstat(d, updatef.qid.path);
		break;
	default:
		return -1;
	}
	return 0;
}

void
fsopen(Req *r)
{
	Updateq *fq;
	
	if(r->fid->qid.path == Qupdate){
		fq = emalloc9p(sizeof(Updateq));
		fq->fid = r->ifcall.fid;
		fq->last = fqtail;
		if(fqtail != nil)
			fqtail->link = fq;
		else
			fqhead = fq; /* assert fqhead == nil */
		fqtail = fq;
		fq->hasread = 0;
		r->fid->aux = fq;
	}
	
	respond(r, nil);
}


void
readspond(Req *r)
{
	Updateq *fq;
	
	fq = r->fid->aux;
	
	if(r->ifcall.count < 1){
		respond(r, "buffer too small");
	}else{
		r->ofcall.count = 1;
		r->ofcall.data[0] = 'y'; /* draw(3) y, but not really */
		fq->hasread = 1;
		respond(r,nil);
	}
}

void
fsread(Req *r)
{
	uvlong path;
	Updateq *fq;
	Readq *rq;
	ulong c, o, n;
	
	path = r->fid->qid.path;
	if(path == Qroot){
		dirread9p(r, rootgen, nil);
		respond(r, nil);
	}else if(path == Qcanvas){
		c = r->ifcall.count;
		o = r->ifcall.offset;
		n = 0;
		if(o < 5*12){
			n += 5*12 - o;
			if(c < n)
				n = c;
			memcpy(r->ofcall.data, imhdr + o, n);
			c -= n;
			o += n;
		}
		o -= 5*12;
		if(o < memimagebytelen(canvas) && c > 0){
			if(c + o > memimagebytelen(canvas))
				c = memimagebytelen(canvas) - o;
			memcpy(r->ofcall.data + n, imdata + o, c);
			n += c;
		}
		r->ofcall.count = n;
		respond(r, nil);
	}else if(path == Qupdate){
		/* if buffer ptr is behind, catchup, otherwise add to queue */
		fq = r->fid->aux;
		if(fq->hasread){
			rq = emalloc9p(sizeof(Readq)); /* potential cache thrasher */
			rq->this = r;
			rq->last = rqtail;
			if(rqtail != nil)
				rqtail->link = rq;
			else
				rqhead = rq; /* assert rqhead == nil */
			rqtail = rq;
			r->aux = rq;
		}else{
			readspond(r);
		}
	}else{
		respond(r, "invalid Qid path");
	}
}

void
fswrite(Req *r)
{
	Readq *rq, *next;
	int e;
	Writeq *wq;
	
	if(r->fid->qid.path != Qcanvas){
		respond(r, "Cannot write to there!");
		return;
	}
	
	/* basic sanitization until I add support for general image types and compression */
	wq = r->fid->aux;
	if(wq == nil){
		if(r->ifcall.count < 60){
			respond(r, "write buffer too small, cannot recover header");
			return;
		}
		if(memcmp(r->ifcall.data, "compressed", 10) == 0){
			respond(r, "compressed images not supported");
			return;
		}
		if(memcmp(r->ifcall.data, "   r8g8b8a8 ", 12) != 0){
			respond(r, "image rejected, only depth supported is r8g8b8a8");
			return;
		}
		/*
		 * avoid atoi madness by just matching the header.
		 * will need to go back to parsing integers in order
		 * to support subrectangle drawing.
		 */
		if(memcmp(r->ifcall.data+12, imhdr+12, 4*12) != 0){
			respond(r, "image rejected, rectangle doesn't match");
			return;
		}
		wq = malloc(sizeof(Writeq));
		if(wq == nil){
			respond(r, "out of memory #1");
			return;
		}
		wq->n = 5*12 + memimagebytelen(diff);
		wq->buf = malloc(wq->n);
		if(wq->buf == nil){
			free(wq);
			respond(r, "out of memory #2");
			return;
		}
		r->fid->aux = wq;
		/* need to adjust for subrectangle drawing */
		wq->r = canvas->r;
		wq->chan = canvas->chan;
		wq->i = 0;
	}
	if(wq->i + r->ifcall.count > wq->n){
		respond(r, "invalid image bytes sent, deleting write request");
		free(wq->buf);
		free(wq);
		return;
	}
	memcpy(wq->buf+wq->i, r->ifcall.data, r->ifcall.count);
	wq->i += r->ifcall.count;
	r->ofcall.count = r->ifcall.count;
	if(wq->i != wq->n){
		respond(r, nil);
		return;
	}
	/* Successful image write. Load, composite, update, and notify. */
	/*
	 * Currently: wq->chan == diff->chan && wq->r == diff->r
	 * To support different image write sizes, a new Memimage will
	 * need to be allocated.
	 */
	e = loadmemimage(diff, diff->r, wq->buf+5*12, wq->n-5*12);
	free(wq->buf);
	free(wq);
	r->fid->aux = nil;
	if(e < 0){ /* assert/guess cannot happen */
		respond(r, "image rejected, error in byte data");
		fprint(2, "whiteboardfs: Invalid Twrite. Please do more testing.\n");
		return;
	}
	respond(r,nil);
	memimagedraw(canvas, canvas->r, diff, ZP, nil, ZP, SoverD);
	imdataupdate();
	/* respond to all waiting reads */
	for(rq = rqhead; rq != nil; rq = next){
		/* respond() calls destroyreq, which frees this rq */
		next = rq->link;
		readspond(rq->this);
	}
	rqhead = nil;
	rqtail = nil;
}

void
fsflush(Req *r)
{
	Readq *rq;
	for(rq = rqhead; rq != nil; rq = rq->link){
		if(r->ifcall.oldtag == rq->this->tag){
			respond(rq->this, "interrupted");
			if(rq->last != nil)
				rq->last->link = rq->link;
			else
				rqhead = rq->link;
			if(rq->link != nil)
				rq->link->last = rq->last;
			else
				rqtail = rq->last;
			respond(r, nil);
			return;
		}
	}
	respond(r, "invalid tag");
}

void
fsdestroyfid(Fid *fid)
{
	Updateq *fq;
	Writeq *wq;
	
	if(fid->aux == nil)
		return;
	if(fid->qid.path == Qupdate){
		fq = fid->aux;
		/* remove self from DLL */
		if(fq->last == nil){
			fqhead = fq->link;
		}else{
			fq->last->link = fq->link;
		}
		if(fq->link == nil){
			fqtail = fq->last;
		}else{
			fq->link->last = fq->last;
		}
		
		free(fq);
		fid->aux = nil;
	}else if(fid->qid.path == Qcanvas){
		wq = fid->aux;
		free(wq->buf);
		free(wq);
		fid->aux = nil;
	}else{ /* assert this branch cannot happen */
		sysfatal("unknown dangling fid->aux, qid=%ulld", fid->qid.path);
	}
}

void
fsdestroyreq(Req *r)
{
	Readq *rq;
	
	if(r->aux == nil)
		return;
	rq = r->aux;
	/* remove self from DLL */
	if(rq->last == nil){
		rqhead = rq->link;
	}else{
		rq->last->link = rq->link;
	}
	if(rq->link == nil){
		rqtail = rq->last;
	}else{
		rq->link->last = rq->last;
	}
	
	free(rq);
	r->aux = nil;
}

Srv fs = {
	.attach = fsattach,
	.walk1 = fswalk1,
	.stat = fsstat,
	.open = fsopen,
	.read = fsread,
	.write = fswrite,
	.flush = fsflush,
	.destroyfid = fsdestroyfid,
	.destroyreq = fsdestroyreq,
};

void
main(int argc, char *argv[])
{
	char *mtpt, *srv;
	int x, y;
	
	mtpt = "/mnt/whiteboard";
	srv = nil;
	x = 256;
	y = 256;
	ARGBEGIN{
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 's':
		srv = EARGF(usage());
		break;
	case 'x':
		x = strtol(EARGF(usage()), nil, 0);
		break;
	case 'y':
		y = strtol(EARGF(usage()), nil, 0);
		break;
	case 'D':
		chatty9p++;
		break;
	default:
		usage();
	}ARGEND;
	
	if(x <= 1)
		exits("x too small");
	if(x >= 1024) /* don't break the network */
		exits("x too big");
	if(y <= 1)
		exits("y too small");
	if(y >= 1024)
		exits("y too big");
	
	if(memimageinit() < 0)
		sysfatal("%r");
	canvas = allocmemimage(Rect(0,0,x,y), RGB24);
	memfillcolor(canvas, 0xA0A0A0FF);
	imhdrupdate();
	diff = allocmemimage(Rect(0,0,x,y), RGBA32);
	
	user = getuser();
	postmountsrv(&fs, srv, mtpt, MREPL);
	
	exits(nil);
}
