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


Memimage *fb;
Memimage *red, *green, *blue;
OBJ *model;
Channel *drawc;
int nprocs;

void resized(void);
uvlong nanosec(void);

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
swap(int *a, int *b)
{
	int t;

	t = *a;
	*a = *b;
	*b = t;
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

Memimage *
modelshader(Sparams *sp)
{
	OBJObject *o;
	OBJElem *e;
	OBJVertex *verts;
	OBJIndexArray *idxtab;
	Triangle3 t;
	Triangle2 st;
	Point3 bc;
	int i;
	uchar cbuf[4];

	verts = model->vertdata[OBJVGeometric].verts;

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

				st.p0 = Pt2((t.p0.x+1)*Dx(fb->r)/2, (t.p0.y+1)*Dy(fb->r)/2, 1);
				st.p1 = Pt2((t.p1.x+1)*Dx(fb->r)/2, (t.p1.y+1)*Dy(fb->r)/2, 1);
				st.p2 = Pt2((t.p2.x+1)*Dx(fb->r)/2, (t.p2.y+1)*Dy(fb->r)/2, 1);

				bc = barycoords(st, Pt2(sp->p.x,sp->p.y,1));
				if(bc.x < 0 || bc.y < 0 || bc.z < 0)
					continue;

				cbuf[0] = 0xFF;
				cbuf[1] = 0xFF*bc.x;
				cbuf[2] = 0xFF*bc.y;
				cbuf[3] = 0xFF*bc.z;

				memfillcolor(sp->frag, *(ulong*)cbuf);
				return sp->frag;
			}
	return nil;
}

void
drawmodel(Memimage *dst)
{
	OBJObject *o;
	OBJElem *e;
	OBJVertex *verts;
	OBJIndexArray *idxtab;
	Triangle3 t;
	Triangle st;
	int i;
	uchar cbuf[4];

	verts = model->vertdata[OBJVGeometric].verts;

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

				st[0] = Pt((t.p0.x+1)*Dx(fb->r)/2, (t.p0.y+1)*Dy(fb->r)/2);
				st[1] = Pt((t.p1.x+1)*Dx(fb->r)/2, (t.p1.y+1)*Dy(fb->r)/2);
				st[2] = Pt((t.p2.x+1)*Dx(fb->r)/2, (t.p2.y+1)*Dy(fb->r)/2);

				/* discard degenerates */
				if(eqpt(st[0], st[1]) || eqpt(st[1], st[2]) || eqpt(st[2], st[0]))
					continue;

				cbuf[0] = 0xFF;
				cbuf[1] = 0xFF*frand();
				cbuf[2] = 0xFF*frand();
				cbuf[3] = 0xFF*frand();

				filltriangle(dst, st[0], st[1], st[2], rgb(*(ulong*)cbuf));
			}
}

void
redraw(void)
{
	lockdisplay(display);
	draw(screen, screen->r, display->black, nil, ZP);
	loadimage(screen, rectaddpt(fb->r, screen->r.min), byteaddr(fb, fb->r.min), bytesperline(fb->r, fb->depth)*Dy(fb->r));
	flushimage(display, 1);
	unlockdisplay(display);
}

void
render(void)
{
	uvlong t0, t1;

	if(model != nil){
//		t0 = nanosec();
//		shade(fb, modelshader);
//		t1 = nanosec();
//		fprint(2, "shader took %lludns\n", t1-t0);

		t0 = nanosec();
		drawmodel(fb);
		t1 = nanosec();
		fprint(2, "drawmodel took %lludns\n", t1-t0);
	}else{
		t0 = nanosec();
		shade(fb, circleshader);
		t1 = nanosec();
		fprint(2, "shader took %lludns\n", t1-t0);

		bresenham(fb, Pt(40,40), Pt(300,300), red);
		bresenham(fb, Pt(80,80), Pt(100,200), red);
		bresenham(fb, Pt(80,80), Pt(200,100), red);

		filltriangle(fb, Pt(30,10), Pt(45, 45), Pt(5, 100), blue);
		triangle(fb, Pt(30,10), Pt(45, 45), Pt(5, 100), red);
		filltriangle(fb, Pt(300,120), Pt(200,350), Pt(50, 210), blue);
		triangle(fb, Pt(300,120), Pt(200,350), Pt(50, 210), red);
		filltriangle(fb, Pt(400,230), Pt(450,180), Pt(150, 320), blue);
		triangle(fb, Pt(400,230), Pt(450,180), Pt(150, 320), red);

		t0 = nanosec();
		shade(fb, triangleshader);
		t1 = nanosec();
		fprint(2, "shader took %lludns\n", t1-t0);
	}
}

void
rmb(Mousectl *, Keyboardctl *)
{
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
	fprint(2, "usage: %s [-n nprocs] [-m objfile]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	Mousectl *mc;
	Keyboardctl *kc;
	Rune r;
	char *mdlpath;

	GEOMfmtinstall();
	mdlpath = nil;
	ARGBEGIN{
	case 'n':
		nprocs = strtoul(EARGF(usage()), nil, 10);
		break;
	case 'm':
		mdlpath = EARGF(usage());
		break;
	default: usage();
	}ARGEND;
	if(argc != 0)
		usage();

	if(nprocs < 1)
		nprocs = strtoul(getenv("NPROC"), nil, 10);

	if(newwindow(nil) < 0)
		sysfatal("newwindow: %r");
	if(initdraw(nil, nil, nil) < 0)
		sysfatal("initdraw: %r");
	if(memimageinit() != 0)
		sysfatal("memimageinit: %r");
	if((mc = initmouse(nil, screen)) == nil)
		sysfatal("initmouse: %r");
	if((kc = initkeyboard(nil)) == nil)
		sysfatal("initkeyboard: %r");

	fb = eallocmemimage(rectsubpt(screen->r, screen->r.min), screen->chan);
	red = rgb(DRed);
	green = rgb(DGreen);
	blue = rgb(DBlue);

	if(mdlpath != nil){
		model = objparse(mdlpath);
		if(model == nil)
			sysfatal("objparse: %r");
	}

	render();

	drawc = chancreate(sizeof(void*), 1);
	display->locking = 1;
	unlockdisplay(display);
	nbsend(drawc, nil);

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
