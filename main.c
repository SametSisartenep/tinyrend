#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#include <draw.h>
#include <memdraw.h>
#include <mouse.h>
#include <keyboard.h>
#include <geometry.h>
#include "libobj/obj.h"

#define HZ2MS(hz)	(1000/(hz))

typedef Point Triangle[3];
typedef struct Sparams Sparams;
typedef struct SUparams SUparams;

/* shader params */
struct Sparams
{
	Memimage *frag;
	Point p;
};

/* shader unit params */
struct SUparams
{
	Memimage *dst;
	Rectangle r;
	int id;
	Channel *donec;
	Memimage *(*shader)(Sparams*);
};


Memimage *fb, *zfb, *curfb;
double *zbuf;
Memimage *red, *green, *blue;
OBJ *model;
Memimage *modeltex;
Channel *drawc;
int nprocs;
int rendering;

char winspec[32];

void resized(void);
uvlong nanosec(void);

int
min(int a, int b)
{
	return a < b? a: b;
}

int
max(int a, int b)
{
	return a > b? a: b;
}

void
swap(int *a, int *b)
{
	int t;

	t = *a;
	*a = *b;
	*b = t;
}

void
memsetd(double *p, double v, usize len)
{
	double *dp;

	for(dp = p; dp < p+len; dp++)
		*dp = v;
}

double
step(double edge, double n)
{
	if(n < edge)
		return 0;
	return 1;
}

double
smoothstep(double edge0, double edge1, double n)
{
	double t;

	t = fclamp((n-edge0)/(edge1-edge0), 0, 1);
	return t*t * (3 - 2*t);
}

void *
emalloc(ulong n)
{
	void *p;

	p = malloc(n);
	if(p == nil)
		sysfatal("malloc: %r");
	setmalloctag(p, getcallerpc(&n));
	return p;
}

void *
erealloc(void *p, ulong n)
{
	void *np;

	np = realloc(p, n);
	if(np == nil){
		if(n == 0)
			return nil;
		sysfatal("realloc: %r");
	}
	if(p == nil)
		setmalloctag(np, getcallerpc(&p));
	else
		setrealloctag(np, getcallerpc(&p));
	return np;
}

Image *
eallocimage(Display *d, Rectangle r, ulong chan, int repl, ulong col)
{
	Image *i;

	i = allocimage(d, r, chan, repl, col);
	if(i == nil)
		sysfatal("allocimage: %r");
	return i;
}

Memimage *
eallocmemimage(Rectangle r, ulong chan)
{
	Memimage *i;

	i = allocmemimage(r, chan);
	if(i == nil)
		sysfatal("allocmemimage: %r");
	memfillcolor(i, DTransparent);
	return i;
}

static void
decproc(void *arg)
{
	int fd, *pfd;

	pfd = arg;
	fd = pfd[2];

	close(pfd[0]);
	dup(fd, 0);
	close(fd);
	dup(pfd[1], 1);
	close(pfd[1]);

	execl("/bin/tga", "tga", "-9t", nil);
	threadexitsall("execl: %r");
}

Memimage *
readtga(char *path)
{
	Memimage *i;
	int fd, pfd[3];

	if(pipe(pfd) < 0)
		sysfatal("pipe: %r");
	fd = open(path, OREAD);
	if(fd < 0)
		sysfatal("open: %r");
	pfd[2] = fd;
	procrfork(decproc, pfd, mainstacksize, RFFDG|RFNAMEG|RFNOTEG);
	close(pfd[1]);
	i = readmemimage(pfd[0]);
	close(pfd[0]);
	close(fd);

	return i;
}

Memimage *
rgb(ulong c)
{
	Memimage *i;

	i = eallocmemimage(Rect(0,0,1,1), screen->chan);
	i->flags |= Frepl;
	i->clipr = Rect(-1e6, -1e6, 1e6, 1e6);
	memfillcolor(i, c);
	return i;
}

void
pixel(Memimage *dst, Point p, Memimage *src)
{
	if(dst == nil || src == nil)
		return;

	memimagedraw(dst, rectaddpt(Rect(0,0,1,1), p), src, ZP, nil, ZP, SoverD);
}

void
bresenham(Memimage *dst, Point p0, Point p1, Memimage *src)
{
	int steep = 0, Δe, e, Δy;
	Point p, dp;

	/* transpose the points */
	if(abs(p0.x-p1.x) < abs(p0.y-p1.y)){
		steep = 1;
		swap(&p0.x, &p0.y);
		swap(&p1.x, &p1.y);
	}

	/* make them left-to-right */
	if(p0.x > p1.x){
		swap(&p0.x, &p1.x);
		swap(&p0.y, &p1.y);
	}

	dp = subpt(p1, p0);
	Δe = 2*abs(dp.y);
	e = 0;
	Δy = p1.y > p0.y? 1: -1;

	for(p = p0; p.x <= p1.x; p.x++){
		if(steep) swap(&p.x, &p.y);
		pixel(dst, p, src);
		if(steep) swap(&p.x, &p.y);

		e += Δe;
		if(e > dp.x){
			p.y += Δy;
			e -= 2*dp.x;
		}
	}
}

int
ycoordsort(void *a, void *b)
{
	return ((Point*)a)->y - ((Point*)b)->y;
}

void
triangle(Memimage *dst, Point p0, Point p1, Point p2, Memimage *src)
{
	Triangle t;

	t[0] = p0;
	t[1] = p1;
	t[2] = p2;

	qsort(t, nelem(t), sizeof(Point), ycoordsort);

	bresenham(dst, t[0], t[1], src);
	bresenham(dst, t[1], t[2], src);
	bresenham(dst, t[2], t[0], green);
}

void
filltriangle(Memimage *dst, Point p0, Point p1, Point p2, Memimage *src)
{
	int y;
	double m₀₂, m₀₁, m₁₂;
	Point dp₀₂, dp₀₁, dp₁₂;
	Triangle t;

	t[0] = p0;
	t[1] = p1;
	t[2] = p2;

	qsort(t, nelem(t), sizeof(Point), ycoordsort);

	dp₀₂ = subpt(t[2], t[0]);
	m₀₂ = dp₀₂.y == 0? 0: (double)dp₀₂.x/dp₀₂.y;
	dp₀₁ = subpt(t[1], t[0]);
	m₀₁ = dp₀₁.y == 0? 0: (double)dp₀₁.x/dp₀₁.y;
	dp₁₂ = subpt(t[2], t[1]);
	m₁₂ = dp₁₂.y == 0? 0: (double)dp₁₂.x/dp₁₂.y;

	/* first half */
	for(y = t[0].y; y <= t[1].y; y++)
		bresenham(dst, Pt(t[0].x + (y-t[0].y)*m₀₂,y), Pt(t[0].x + (y-t[0].y)*m₀₁,y), src);
	/* second half */
	for(; y <= t[2].y; y++)
		bresenham(dst, Pt(t[0].x + (y-t[0].y)*m₀₂,y), Pt(t[1].x + (y-t[1].y)*m₁₂,y), src);
}

void
shaderunit(void *arg)
{
	SUparams *params;
	Sparams sp;
	Point p;
	Memimage *c;

	params = arg;
	sp.frag = rgb(DBlack);

	threadsetname("shader unit #%d", params->id);

	for(p.y = params->r.min.y; p.y < params->r.max.y; p.y++)
		for(p.x = params->r.min.x; p.x < params->r.max.x; p.x++){
			sp.p = p;
			if((c = params->shader(&sp)) != nil)
				pixel(params->dst, p, c);
		}

	freememimage(sp.frag);
	sendp(params->donec, nil);
	free(params);
	threadexits(nil);
}

void
shade(Memimage *dst, Memimage *(*shader)(Sparams*))
{
	int i;
	Point dim;
	SUparams *params;
	Channel *donec;

	/* shitty approach until i find a better algo */
	dim.x = Dx(dst->r)/nprocs;

	donec = chancreate(sizeof(void*), 0);

	for(i = 0; i < nprocs; i++){
		params = emalloc(sizeof *params);
		params->dst = dst;
		params->r = Rect(i*dim.x,0,min((i+1)*dim.x, dst->r.max.x),dst->r.max.y);
		params->id = i;
		params->donec = donec;
		params->shader = shader;
		proccreate(shaderunit, params, mainstacksize);
		fprint(2, "spawned su %d for %R\n", params->id, params->r);
	}

	while(i--)
		recvp(donec);
	chanfree(donec);
}

Memimage *
triangleshader(Sparams *sp)
{
	Triangle2 t;
	Rectangle bbox;
	Point3 bc;
	uchar cbuf[4];

	t.p0 = Pt2(240,200,1);
	t.p1 = Pt2(400,40,1);
	t.p2 = Pt2(240,40,1);

	bbox = Rect(
		min(min(t.p0.x, t.p1.x), t.p2.x), min(min(t.p0.y, t.p1.y), t.p2.y),
		max(max(t.p0.x, t.p1.x), t.p2.x), max(max(t.p0.y, t.p1.y), t.p2.y)
	);
	if(!ptinrect(sp->p, bbox))
		return nil;

	bc = barycoords(t, Pt2(sp->p.x,sp->p.y,1));
	if(bc.x < 0 || bc.y < 0 || bc.z < 0)
		return nil;

	cbuf[0] = 0xFF;
	cbuf[1] = 0xFF*bc.z;
	cbuf[2] = 0xFF*bc.y;
	cbuf[3] = 0xFF*bc.x;
	memfillcolor(sp->frag, *(ulong*)cbuf);
	return sp->frag;
}

Memimage *
circleshader(Sparams *sp)
{
	Point2 uv;
	double r;
	uchar cbuf[4];

	uv = Pt2(sp->p.x,sp->p.y,1);
	uv.x /= Dx(fb->r);
	uv.y /= Dy(fb->r);
	r = 0.3;

	if(vec2len(subpt2(uv, Vec2(0.5,0.5))) > r)
		return nil;

	cbuf[0] = 0xFF;
	cbuf[1] = 0;
	cbuf[2] = 0xFF*uv.y;
	cbuf[3] = 0xFF*uv.x;

	memfillcolor(sp->frag, *(ulong*)cbuf);
	return sp->frag;
}

/* some shaping functions from The Book of Shaders, Chapter 5 */
Memimage *
sfshader(Sparams *sp)
{
	Point2 uv;
	double y, pct;
	uchar cbuf[4];

	uv = Pt2(sp->p.x,sp->p.y,1);
	uv.x /= Dx(fb->r);
	uv.y /= Dy(fb->r);
	uv.y = 1 - uv.y;		/* make [0 0] the bottom-left corner */

//	y = step(0.5, uv.x);
	y = pow(uv.x, 5);
//	y = sin(uv.x);
//	y = smoothstep(0.1, 0.9, uv.x);
	pct = smoothstep(y-0.02, y, uv.y) - smoothstep(y, y+0.02, uv.y);

	cbuf[0] = 0xFF;
	cbuf[1] = 0xFF*flerp(y, 0, pct);
	cbuf[2] = 0xFF*flerp(y, 1, pct);
	cbuf[3] = 0xFF*flerp(y, 0, pct);

	memfillcolor(sp->frag, *(ulong*)cbuf);
	return sp->frag;
}

Memimage *
modelshader(Sparams *sp)
{
	OBJObject *o;
	OBJElem *e;
	OBJVertex *verts, *tverts;		/* geometric and texture vertices */
	OBJIndexArray *idxtab;
	Triangle3 t;
	Triangle2 st, tt;			/* screen and texture triangles */
	Rectangle bbox;
	Point3 bc, n;				/* barycentric coords and surface normal */
	static Point3 light = {0,0,-1,0};	/* global light field */
	Point tp;				/* texture point */
	int i;
	uchar cbuf[4];
	double z, intensity;

	verts = model->vertdata[OBJVGeometric].verts;
	tverts = model->vertdata[OBJVTexture].verts;

	for(i = 0; i < nelem(model->objtab); i++)
		for(o = model->objtab[i]; o != nil; o = o->next)
			for(e = o->child; e != nil; e = e->next){
				idxtab = &e->indextab[OBJVGeometric];

				/* discard non-triangles */
				if(e->type != OBJEFace || idxtab->nindex != 3)
					continue;

				t.p0 = Pt3(verts[idxtab->indices[0]].x,verts[idxtab->indices[0]].y,verts[idxtab->indices[0]].z,verts[idxtab->indices[0]].w);
				t.p1 = Pt3(verts[idxtab->indices[1]].x,verts[idxtab->indices[1]].y,verts[idxtab->indices[1]].z,verts[idxtab->indices[1]].w);
				t.p2 = Pt3(verts[idxtab->indices[2]].x,verts[idxtab->indices[2]].y,verts[idxtab->indices[2]].z,verts[idxtab->indices[2]].w);

				st.p0 = Pt2((t.p0.x+1)*Dx(fb->r)/2, (-t.p0.y+1)*Dy(fb->r)/2, 1);
				st.p1 = Pt2((t.p1.x+1)*Dx(fb->r)/2, (-t.p1.y+1)*Dy(fb->r)/2, 1);
				st.p2 = Pt2((t.p2.x+1)*Dx(fb->r)/2, (-t.p2.y+1)*Dy(fb->r)/2, 1);

				bbox = Rect(
					min(min(st.p0.x, st.p1.x), st.p2.x), min(min(st.p0.y, st.p1.y), st.p2.y),
					max(max(st.p0.x, st.p1.x), st.p2.x)+1, max(max(st.p0.y, st.p1.y), st.p2.y)+1
				);
				if(!ptinrect(sp->p, bbox))
					continue;

				bc = barycoords(st, Pt2(sp->p.x,sp->p.y,1));
				if(bc.x < 0 || bc.y < 0 || bc.z < 0)
					continue;

				z = t.p0.z*bc.x + t.p1.z*bc.y + t.p2.z*bc.z;
				if(z <= zbuf[sp->p.x+sp->p.y*Dx(fb->r)])
					continue;
				zbuf[sp->p.x+sp->p.y*Dx(fb->r)] = z;

				cbuf[0] = 0xFF;
				cbuf[1] = 0xFF*z;
				cbuf[2] = 0xFF*z;
				cbuf[3] = 0xFF*z;
				memfillcolor(sp->frag, *(ulong*)cbuf);
				pixel(zfb, sp->p, sp->frag);

				n = normvec3(crossvec3(subpt3(t.p2, t.p0), subpt3(t.p1, t.p0)));
				intensity = dotvec3(n, light);
				/* back-face culling */
				if(intensity < 0)
					continue;

				idxtab = &e->indextab[OBJVTexture];
				if(modeltex != nil && idxtab->nindex == 3){
					tt.p0 = Pt2(tverts[idxtab->indices[0]].u, tverts[idxtab->indices[0]].v, 1);
					tt.p1 = Pt2(tverts[idxtab->indices[1]].u, tverts[idxtab->indices[1]].v, 1);
					tt.p2 = Pt2(tverts[idxtab->indices[2]].u, tverts[idxtab->indices[2]].v, 1);

					tt.p0 = mulpt2(tt.p0, bc.x);
					tt.p1 = mulpt2(tt.p1, bc.y);
					tt.p2 = mulpt2(tt.p2, bc.z);

					tp.x = (tt.p0.x + tt.p1.x + tt.p2.x)*Dx(modeltex->r);
					tp.y = Dy(modeltex->r)-(tt.p0.y + tt.p1.y + tt.p2.y)*Dy(modeltex->r);

					cbuf[0] = 0xFF;
					unloadmemimage(modeltex, rectaddpt(Rect(0,0,1,1), tp), cbuf+1, sizeof cbuf - 1);
				}else{
					cbuf[0] = 0xFF;
					cbuf[1] = 0xFF;
					cbuf[2] = 0xFF;
					cbuf[3] = 0xFF;
				}
				cbuf[1] *= intensity;
				cbuf[2] *= intensity;
				cbuf[3] *= intensity;

				memfillcolor(sp->frag, *(ulong*)cbuf);
				return sp->frag;
			}
	return nil;
}

void
redraw(void)
{
	lockdisplay(display);
	draw(screen, screen->r, display->black, nil, ZP);
	loadimage(screen, rectaddpt(curfb->r, screen->r.min), byteaddr(curfb, curfb->r.min), bytesperline(curfb->r, curfb->depth)*Dy(curfb->r));
	flushimage(display, 1);
	unlockdisplay(display);
}

void
render(void)
{
	uvlong t0, t1;

	t0 = nanosec();
	shade(fb, model != nil? modelshader: sfshader);
	t1 = nanosec();
	fprint(2, "shader took %lludns\n", t1-t0);
}

void
renderer(void *)
{
	threadsetname("renderer");

	render();
	rendering = 0;
	nbsendp(drawc, nil);
	threadexits(nil);
}

void
scrsync(void *)
{
	Ioproc *io;

	threadsetname("scrsync");

	io = ioproc();
	while(rendering){
		nbsendp(drawc, nil);
		iosleep(io, 2000);
	}
	closeioproc(io);
	threadexits(nil);
}

static char *
genrmbmenuitem(int idx)
{
	enum {
		TOGGLEZBUF,
	};

	if(idx == TOGGLEZBUF)
		return curfb == zfb? "hide z-buffer": "show z-buffer";
	return nil;
}

void
rmb(Mousectl *mc, Keyboardctl *)
{
	enum {
		TOGGLEZBUF,
	};
	static Menu menu = { .gen = genrmbmenuitem };

	switch(menuhit(3, mc, &menu, _screen)){
	case TOGGLEZBUF:
		curfb = curfb == fb? zfb: fb;
		break;
	}
	nbsendp(drawc, nil);
}

void
lmb(Mousectl *, Keyboardctl *)
{
}

void
mouse(Mousectl *mc, Keyboardctl *kc)
{
	if((mc->buttons&1) != 0)
		lmb(mc, kc);
	if((mc->buttons&4) != 0)
		rmb(mc, kc);
}

void
key(Rune r)
{
	switch(r){
	case Kdel:
	case 'q':
		threadexitsall(nil);
	}
}

void
usage(void)
{
	fprint(2, "usage: %s [-n nprocs] [-m objfile] [-t texfile]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	Mousectl *mc;
	Keyboardctl *kc;
	Rune r;
	char *mdlpath, *texpath;

	GEOMfmtinstall();
	mdlpath = nil;
	texpath = nil;
	ARGBEGIN{
	case 'n':
		nprocs = strtoul(EARGF(usage()), nil, 10);
		break;
	case 'm':
		mdlpath = EARGF(usage());
		break;
	case 't':
		texpath = EARGF(usage());
		break;
	default: usage();
	}ARGEND;
	if(argc != 0)
		usage();

	if(nprocs < 1)
		nprocs = strtoul(getenv("NPROC"), nil, 10);

	snprint(winspec, sizeof winspec, "-dx %d -dy %d", 800, 800);
	if(newwindow(winspec) < 0)
		sysfatal("newwindow: %r");
	if(initdraw(nil, nil, "tinyrend") < 0)
		sysfatal("initdraw: %r");
	if(memimageinit() != 0)
		sysfatal("memimageinit: %r");
	if((mc = initmouse(nil, screen)) == nil)
		sysfatal("initmouse: %r");
	if((kc = initkeyboard(nil)) == nil)
		sysfatal("initkeyboard: %r");

	fb = eallocmemimage(rectsubpt(screen->r, screen->r.min), screen->chan);
	zbuf = emalloc(Dx(fb->r)*Dy(fb->r)*sizeof(double));
	memsetd(zbuf, Inf(-1), Dx(fb->r)*Dy(fb->r));
	zfb = eallocmemimage(fb->r, fb->chan);
	curfb = fb;
	red = rgb(DRed);
	green = rgb(DGreen);
	blue = rgb(DBlue);

	if(mdlpath != nil && (model = objparse(mdlpath)) == nil)
		sysfatal("objparse: %r");
	if(texpath != nil && (modeltex = readtga(texpath)) == nil)
		sysfatal("readtga: %r");

	drawc = chancreate(sizeof(void*), 1);
	display->locking = 1;
	unlockdisplay(display);

	rendering = 1;
	proccreate(renderer, nil, mainstacksize);
	threadcreate(scrsync, nil, mainstacksize);

	for(;;){
		enum { MOUSE, RESIZE, KEYBOARD, DRAW };
		Alt a[] = {
			{mc->c, &mc->Mouse, CHANRCV},
			{mc->resizec, nil, CHANRCV},
			{kc->c, &r, CHANRCV},
			{drawc, nil, CHANRCV},
			{nil, nil, CHANEND}
		};

		switch(alt(a)){
		case MOUSE:
			mouse(mc, kc);
			break;
		case RESIZE:
			resized();
			break;
		case KEYBOARD:
			key(r);
			break;
		case DRAW:
			redraw();
			break;
		}
	}
}

void
resized(void)
{
	lockdisplay(display);
	if(getwindow(display, Refnone) < 0)
		sysfatal("couldn't resize");
	unlockdisplay(display);
	nbsend(drawc, nil);
}
