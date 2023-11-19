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
	OBJElem **b, **e;
	int id;
	Channel *donec;
	Memimage *(*shader)(Sparams*);
};


Memimage *screenfb, *fb, *zfb, *nfb, *curfb;
double *zbuf;
Lock zbuflk;
Memimage *red, *green, *blue;
OBJ *model;
Memimage *modeltex;
Channel *drawc;
int nprocs;
int rendering;
int flag2;
int shownormals;

char winspec[32];
Point3 camera = {0,0,3,1};
Matrix3 proj = {
	1,  0, 0, 0,
	0, -1, 0, 0,
	0,  0, 1, 0,
	0,  0, 0, 1,
}, view = {
	800/2.0, 0, 0, 800/2.0,
	0, 800/2.0, 0, 800/2.0,
	0, 0, 1/2.0, 1/2.0,
	0, 0, 0, 1,
}, rota;

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
swappt2(Point2 *a, Point2 *b)
{
	Point2 t;

	t = *a;
	*a = *b;
	*b = t;
}

void
swappt3(Point3 *a, Point3 *b)
{
	Point3 t;

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
filltriangle2(Memimage *dst, Triangle3 st, Triangle3 nt, Triangle2 tt, double intensity, Memimage *frag)
{
	Rectangle bbox;
	Point p, tp;
	Triangle2 st₂, tt₂;
	Point3 bc;
	double z;
	uchar cbuf[4];

	bbox = Rect(
		min(min(st.p0.x, st.p1.x), st.p2.x), min(min(st.p0.y, st.p1.y), st.p2.y),
		max(max(st.p0.x, st.p1.x), st.p2.x)+1, max(max(st.p0.y, st.p1.y), st.p2.y)+1
	);
	st₂.p0 = Pt2(st.p0.x, st.p0.y, 1);
	st₂.p1 = Pt2(st.p1.x, st.p1.y, 1);
	st₂.p2 = Pt2(st.p2.x, st.p2.y, 1);
	cbuf[0] = 0xFF;

	for(p.y = bbox.min.y; p.y < bbox.max.y; p.y++)
		for(p.x = bbox.min.x; p.x < bbox.max.x; p.x++){
			bc = barycoords(st₂, Pt2(p.x,p.y,1));
			if(bc.x < 0 || bc.y < 0 || bc.z < 0)
				continue;

			z = st.p0.z*bc.x + st.p1.z*bc.y + st.p2.z*bc.z;
			lock(&zbuflk);
			if(z <= zbuf[p.x + p.y*Dx(dst->r)]){
				unlock(&zbuflk);
				continue;
			}
			zbuf[p.x + p.y*Dx(dst->r)] = z;

			cbuf[1] = 0xFF*z;
			cbuf[2] = 0xFF*z;
			cbuf[3] = 0xFF*z;
			memfillcolor(frag, *(ulong*)cbuf);
			pixel(zfb, p, frag);
			unlock(&zbuflk);

			cbuf[0] = 0xFF;
			if(tt.p0.w != 0 && tt.p1.w != 0 && tt.p2.w != 0){
				tt₂.p0 = mulpt2(tt.p0, bc.x);
				tt₂.p1 = mulpt2(tt.p1, bc.y);
				tt₂.p2 = mulpt2(tt.p2, bc.z);

				tp.x = (tt₂.p0.x + tt₂.p1.x + tt₂.p2.x)*Dx(modeltex->r);
				tp.y = (1 - (tt₂.p0.y + tt₂.p1.y + tt₂.p2.y))*Dy(modeltex->r);

				unloadmemimage(modeltex, rectaddpt(Rect(0,0,1,1), tp), cbuf+1, sizeof cbuf - 1);
			}else
				memset(cbuf+1, 0xFF, sizeof cbuf - 1);

			cbuf[1] *= intensity;
			cbuf[2] *= intensity;
			cbuf[3] *= intensity;

			memfillcolor(frag, *(ulong*)cbuf);
			pixel(dst, p, frag);
		}
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

void
shaderunit2(void *arg)
{
	SUparams *params;
	Sparams sp;
	OBJVertex *verts, *tverts, *nverts;	/* geometric, texture and normals vertices */
	OBJIndexArray *idxtab;
	OBJElem **ep;
	Triangle3 t, st, nt;			/* world-, screen-space and normals triangles */
	Triangle2 tt;				/* texture triangle */
	Point3 n;				/* surface normal */
	static Point3 light = {0,0,-1,0};	/* global light field */
	double intensity;
	Point3 np0, np1;

	params = arg;
	sp.frag = rgb(DBlack);

	threadsetname("shader unit #%d", params->id);

	verts = model->vertdata[OBJVGeometric].verts;
	tverts = model->vertdata[OBJVTexture].verts;
	nverts = model->vertdata[OBJVNormal].verts;

	for(ep = params->b; ep != params->e; ep++){
		idxtab = &(*ep)->indextab[OBJVGeometric];

		t.p0 = Pt3(verts[idxtab->indices[0]].x,verts[idxtab->indices[0]].y,verts[idxtab->indices[0]].z,verts[idxtab->indices[0]].w);
		t.p1 = Pt3(verts[idxtab->indices[1]].x,verts[idxtab->indices[1]].y,verts[idxtab->indices[1]].z,verts[idxtab->indices[1]].w);
		t.p2 = Pt3(verts[idxtab->indices[2]].x,verts[idxtab->indices[2]].y,verts[idxtab->indices[2]].z,verts[idxtab->indices[2]].w);

		st.p0 = xform3(t.p0, view);
		st.p1 = xform3(t.p1, view);
		st.p2 = xform3(t.p2, view);
		st.p0 = divpt3(st.p0, st.p0.w);
		st.p1 = divpt3(st.p1, st.p1.w);
		st.p2 = divpt3(st.p2, st.p2.w);

		n = normvec3(crossvec3(subpt3(t.p2, t.p0), subpt3(t.p1, t.p0)));
		intensity = dotvec3(n, light);
		/* back-face culling */
		if(intensity <= 0)
			continue;

		np0 = centroid3(st);
		np1 = addpt3(np0, mulpt3(n, 10));
		bresenham(nfb, Pt(np0.x,np0.y), Pt(np1.x,np1.y), green);

		idxtab = &(*ep)->indextab[OBJVNormal];
		if(modeltex != nil && idxtab->nindex == 3){
			nt.p0 = Vec3(nverts[idxtab->indices[0]].i, nverts[idxtab->indices[0]].j, nverts[idxtab->indices[0]].k);
			nt.p1 = Vec3(nverts[idxtab->indices[1]].i, nverts[idxtab->indices[1]].j, nverts[idxtab->indices[1]].k);
			nt.p2 = Vec3(nverts[idxtab->indices[2]].i, nverts[idxtab->indices[2]].j, nverts[idxtab->indices[2]].k);
		}else
			memset(&nt, 0, sizeof nt);

		idxtab = &(*ep)->indextab[OBJVTexture];
		if(modeltex != nil && idxtab->nindex == 3){
			tt.p0 = Pt2(tverts[idxtab->indices[0]].u, tverts[idxtab->indices[0]].v, 1);
			tt.p1 = Pt2(tverts[idxtab->indices[1]].u, tverts[idxtab->indices[1]].v, 1);
			tt.p2 = Pt2(tverts[idxtab->indices[2]].u, tverts[idxtab->indices[2]].v, 1);
		}else
			memset(&tt, 0, sizeof tt);

		filltriangle2(params->dst, st, nt, tt, intensity, sp.frag);
	}

	freememimage(sp.frag);
	sendp(params->donec, nil);
	free(params);
	threadexits(nil);
}

void
shade2(Memimage *dst)
{
	int i, nelems, nparts;
	OBJObject *o;
	OBJElem **elems, *e;
	OBJIndexArray *idxtab;
	SUparams *params;
	Channel *donec;

	elems = nil;
	nelems = 0;
	for(i = 0; i < nelem(model->objtab); i++)
		for(o = model->objtab[i]; o != nil; o = o->next)
			for(e = o->child; e != nil; e = e->next){
				idxtab = &e->indextab[OBJVGeometric];
				/* discard non-triangles */
				if(e->type != OBJEFace || idxtab->nindex != 3)
					continue;
				elems = erealloc(elems, ++nelems*sizeof(*elems));
				elems[nelems-1] = e;
			}
	nparts = nelems/nprocs;

	donec = chancreate(sizeof(void*), 0);

	for(i = 0; i < nprocs; i++){
		params = emalloc(sizeof *params);
		params->dst = dst;
		params->b = &elems[i*nparts];
		params->e = params->b + nparts;
		params->id = i;
		params->donec = donec;
		proccreate(shaderunit2, params, mainstacksize);
		fprint(2, "spawned su %d for elems %d to %d\n", params->id, i*nparts, i*nparts+nparts);
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
	Triangle3 t, t₂;
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
	cbuf[0] = 0xFF;

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

				t.p0 = xform3(t.p0, rota);
				t.p1 = xform3(t.p1, rota);
				t.p2 = xform3(t.p2, rota);

				t₂.p0 = xform3(t.p0, view);
				t₂.p1 = xform3(t.p1, view);
				t₂.p2 = xform3(t.p2, view);
				t₂.p0 = divpt3(t₂.p0, t₂.p0.w);
				t₂.p1 = divpt3(t₂.p1, t₂.p1.w);
				t₂.p2 = divpt3(t₂.p2, t₂.p2.w);

				st.p0 = Pt2(t₂.p0.x, t₂.p0.y, 1);
				st.p1 = Pt2(t₂.p1.x, t₂.p1.y, 1);
				st.p2 = Pt2(t₂.p2.x, t₂.p2.y, 1);

				bbox = Rect(
					min(min(st.p0.x, st.p1.x), st.p2.x), min(min(st.p0.y, st.p1.y), st.p2.y),
					max(max(st.p0.x, st.p1.x), st.p2.x)+1, max(max(st.p0.y, st.p1.y), st.p2.y)+1
				);
				if(!ptinrect(sp->p, bbox))
					continue;

				bc = barycoords(st, Pt2(sp->p.x,sp->p.y,1));
				if(bc.x < 0 || bc.y < 0 || bc.z < 0)
					continue;

				z = t₂.p0.z*bc.x + t₂.p1.z*bc.y + t₂.p2.z*bc.z;
				if(z <= zbuf[sp->p.x+sp->p.y*Dx(fb->r)])
					continue;
				zbuf[sp->p.x+sp->p.y*Dx(fb->r)] = z;

				cbuf[1] = 0xFF*z;
				cbuf[2] = 0xFF*z;
				cbuf[3] = 0xFF*z;
				memfillcolor(sp->frag, *(ulong*)cbuf);
				pixel(zfb, sp->p, sp->frag);

				n = normvec3(crossvec3(subpt3(t.p2, t.p0), subpt3(t.p1, t.p0)));
				intensity = dotvec3(n, light);
				/* back-face culling */
				if(intensity <= 0)
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
					tp.y = (1 - (tt.p0.y + tt.p1.y + tt.p2.y))*Dy(modeltex->r);

					unloadmemimage(modeltex, rectaddpt(Rect(0,0,1,1), tp), cbuf+1, sizeof cbuf - 1);
				}else{
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
	memfillcolor(screenfb, DBlack);
	memimagedraw(screenfb, screenfb->r, curfb, ZP, nil, ZP, SoverD);
	if(shownormals)
		memimagedraw(screenfb, screenfb->r, nfb, ZP, nil, ZP, SoverD);
	loadimage(screen, rectaddpt(screenfb->r, screen->r.min), byteaddr(screenfb, screenfb->r.min), bytesperline(screenfb->r, screenfb->depth)*Dy(screenfb->r));
	flushimage(display, 1);
	unlockdisplay(display);
}

void
render(void)
{
	uvlong t0, t1;

	t0 = nanosec();
	if(flag2)
		shade2(fb);
	else
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
		TOGGLENORM,
	};

	switch(idx){
	case TOGGLEZBUF:
		return curfb == zfb? "hide z-buffer": "show z-buffer";
	case TOGGLENORM:
		return shownormals? "hide normals": "show normals";
	}
	return nil;
}

void
rmb(Mousectl *mc, Keyboardctl *)
{
	enum {
		TOGGLEZBUF,
		TOGGLENORM,
	};
	static Menu menu = { .gen = genrmbmenuitem };

	switch(menuhit(3, mc, &menu, _screen)){
	case TOGGLEZBUF:
		curfb = curfb == fb? zfb: fb;
		break;
	case TOGGLENORM:
		shownormals ^= 1;
		break;
	}
	nbsendp(drawc, nil);
}

void
lmb(Mousectl *mc, Keyboardctl *)
{
	Point p;

	p = subpt(mc->xy, screen->r.min);
	fprint(2, "p %P z %g\n", p, zbuf[p.x + p.y*Dx(fb->r)]);
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
	fprint(2, "usage: %s [-2] [-n nprocs] [-m objfile] [-t texfile] [-a yrotangle] [-z camzpos]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	Mousectl *mc;
	Keyboardctl *kc;
	Rune r;
	char *mdlpath, *texpath;
	double θ;

	GEOMfmtinstall();
	mdlpath = nil;
	texpath = nil;
	θ = 0;
	ARGBEGIN{
	case '2':
		flag2++;
		break;
	case 'n':
		nprocs = strtoul(EARGF(usage()), nil, 10);
		break;
	case 'm':
		mdlpath = EARGF(usage());
		break;
	case 't':
		texpath = EARGF(usage());
		break;
	case 'a':
		θ = strtoul(EARGF(usage()), nil, 10)*DEG;
		break;
	case 'z':
		camera.z = strtod(EARGF(usage()), nil);
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

	screenfb = eallocmemimage(rectsubpt(screen->r, screen->r.min), screen->chan);
	fb = eallocmemimage(screenfb->r, RGBA32);
	zbuf = emalloc(Dx(fb->r)*Dy(fb->r)*sizeof(double));
	memsetd(zbuf, Inf(-1), Dx(fb->r)*Dy(fb->r));
	zfb = eallocmemimage(fb->r, fb->chan);
	curfb = fb;
	nfb = eallocmemimage(fb->r, fb->chan);
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

	proj[3][2] = -1.0/camera.z;
	Matrix3 yrot = {
		cos(θ), 0, -sin(θ), 0,
		0, 1, 0, 0,
		sin(θ), 0, cos(θ), 0,
		0, 0, 0, 1,
	};
	identity3(rota);
	mulm3(rota, yrot);
	mulm3(proj, rota);
	mulm3(view, proj);
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
