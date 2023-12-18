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
typedef struct VSparams VSparams;
typedef struct FSparams FSparams;
typedef struct SUparams SUparams;

/* shader params */
struct VSparams
{
	SUparams *su;
	Point3 *p;
	Point3 *n;
	uint idx;
};

struct FSparams
{
	SUparams *su;
	Memimage *frag;
	Point p;
	Point3 bc;
	uchar *cbuf;
};

/* shader unit params */
struct SUparams
{
	Memimage *dst;
	OBJElem **b, **e;
	int id;
	Channel *donec;

	double var_intensity[3];

	uvlong uni_time;

	Point3 (*vshader)(VSparams*);
	Memimage *(*fshader)(FSparams*);
};

typedef struct Shader Shader;
struct Shader
{
	char *name;
	Point3 (*vshader)(VSparams*);
	Memimage *(*fshader)(FSparams*);
};

typedef struct Stats Stats;
struct Stats
{
	uvlong min, avg, max, acc, n, v;
};


Stats fps;
Memimage *screenfb, *fb, *zfb, *nfb, *curfb;
double *zbuf;
Lock zbuflk;
Memimage *red, *green, *blue;
OBJ *model;
Memimage *modeltex;
Channel *drawc, *donedrawc;
int nprocs;
int rendering;
int shownormals;

char winspec[32];
Point3 light = {0,-1,1,1};	/* global directional light */
Point3 camera = {0,0,3,1};
Point3 center = {0,0,0,1};
Point3 up = {0,1,0,0};
Matrix3 view, proj, rota;
double θ, ω;

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

double
fmin(double a, double b)
{
	return a < b? a: b;
}

double
fmax(double a, double b)
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

void
updatestats(Stats *s, uvlong v)
{
	s->v = v;
	s->n++;
	s->acc += v;
	s->avg = s->acc/s->n;
	s->min = v < s->min || s->n == 1? v: s->min;
	s->max = v > s->max || s->n == 1? v: s->max;
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
	bresenham(dst, t[2], t[0], src);
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
viewport(Rectangle r)
{
	identity3(view);
	view[0][3] = r.max.x/2.0;
	view[1][3] = r.max.y/2.0;
	view[2][3] = 1.0/2.0;
	view[0][0] = Dx(r)/2.0;
	view[1][1] = -Dy(r)/2.0;
	view[2][2] = 1.0/2.0;
}

void
projection(double c)
{
	identity3(proj);
	proj[3][2] = c;
}

void
lookat(Point3 eye, Point3 o, Point3 up)
{
	Point3 x, y, z;

	z = normvec3(subpt3(eye, o));
	x = normvec3(crossvec3(up, z));
	y = normvec3(crossvec3(z, x));
	rota[0][0] = x.x; rota[0][1] = x.y; rota[0][2] = x.z; rota[0][3] = -o.x;
	rota[1][0] = y.x; rota[1][1] = y.y; rota[1][2] = y.z; rota[1][3] = -o.y;
	rota[2][0] = z.x; rota[2][1] = z.y; rota[2][2] = z.z; rota[2][3] = -o.z;
}

Point3
vertshader(VSparams *sp)
{
	Matrix3 yrot = {
		cos(θ+fmod(ω*sp->su->uni_time/1e9, 2*PI)), 0, -sin(θ+fmod(ω*sp->su->uni_time/1e9, 2*PI)), 0,
		0, 1, 0, 0,
		sin(θ+fmod(ω*sp->su->uni_time/1e9, 2*PI)), 0, cos(θ+fmod(ω*sp->su->uni_time/1e9, 2*PI)), 0,
		0, 0, 0, 1,
	}, M, V;

	identity3(M);
	identity3(V);
	mulm3(M, rota);
	mulm3(M, yrot);
	mulm3(V, view);
	mulm3(V, M);

	*sp->n = xform3(*sp->n, M);
	sp->su->var_intensity[sp->idx] = fmax(0, dotvec3(*sp->n, light));
	*sp->p = xform3(*sp->p, V);
	return *sp->p;
}

Memimage *
gouraudshader(FSparams *sp)
{
	double intens;

	intens = dotvec3(Vec3(sp->su->var_intensity[0], sp->su->var_intensity[1], sp->su->var_intensity[2]), sp->bc);
	sp->cbuf[1] *= intens;
	sp->cbuf[2] *= intens;
	sp->cbuf[3] *= intens;
	memfillcolor(sp->frag, *(ulong*)sp->cbuf);

	return sp->frag;
}

Memimage *
toonshader(FSparams *sp)
{
	double intens;

	intens = dotvec3(Vec3(sp->su->var_intensity[0], sp->su->var_intensity[1], sp->su->var_intensity[2]), sp->bc);
	intens = intens > 0.85? 1: intens > 0.60? 0.80: intens > 0.45? 0.60: intens > 0.30? 0.45: intens > 0.15? 0.30: 0;
	sp->cbuf[1] = 0;
	sp->cbuf[2] = 155*intens;
	sp->cbuf[3] = 255*intens;
	memfillcolor(sp->frag, *(ulong*)sp->cbuf);

	return sp->frag;
}

void
rasterize(SUparams *params, Triangle3 st, Triangle2 tt, Memimage *frag)
{
	FSparams fsp;
	Triangle2 st₂, tt₂;
	Rectangle bbox;
	Point p, tp;
	Point3 bc;
	double z, w, depth;
	uchar cbuf[4];

	st₂.p0 = Pt2(st.p0.x/st.p0.w, st.p0.y/st.p0.w, 1);
	st₂.p1 = Pt2(st.p1.x/st.p1.w, st.p1.y/st.p1.w, 1);
	st₂.p2 = Pt2(st.p2.x/st.p2.w, st.p2.y/st.p2.w, 1);
	/* find the triangle's bbox and clip it against the fb */
	bbox = Rect(
		min(min(st₂.p0.x, st₂.p1.x), st₂.p2.x), min(min(st₂.p0.y, st₂.p1.y), st₂.p2.y),
		max(max(st₂.p0.x, st₂.p1.x), st₂.p2.x)+1, max(max(st₂.p0.y, st₂.p1.y), st₂.p2.y)+1
	);
	bbox.min.x = max(bbox.min.x, fb->r.min.x); bbox.min.y = max(bbox.min.y, fb->r.min.y);
	bbox.max.x = min(bbox.max.x, fb->r.max.x); bbox.max.y = min(bbox.max.y, fb->r.max.y);
	cbuf[0] = 0xFF;
	fsp.su = params;
	fsp.frag = frag;
	fsp.cbuf = cbuf;

	for(p.y = bbox.min.y; p.y < bbox.max.y; p.y++)
		for(p.x = bbox.min.x; p.x < bbox.max.x; p.x++){
			bc = barycoords(st₂, Pt2(p.x,p.y,1));
			if(bc.x < 0 || bc.y < 0 || bc.z < 0)
				continue;

			z = st.p0.z*bc.x + st.p1.z*bc.y + st.p2.z*bc.z;
			w = st.p0.w*bc.x + st.p1.w*bc.y + st.p2.w*bc.z;
			depth = fclamp(z/w, 0, 1);
			lock(&zbuflk);
			if(depth <= zbuf[p.x + p.y*Dx(params->dst->r)]){
				unlock(&zbuflk);
				continue;
			}
			zbuf[p.x + p.y*Dx(params->dst->r)] = depth;

			cbuf[1] = 0xFF*depth;
			cbuf[2] = 0xFF*depth;
			cbuf[3] = 0xFF*depth;
			memfillcolor(frag, *(ulong*)cbuf);
			pixel(zfb, p, frag);
			unlock(&zbuflk);

			cbuf[0] = 0xFF;
			if((tt.p0.w + tt.p1.w + tt.p2.w) != 0){
				tt₂.p0 = mulpt2(tt.p0, bc.x);
				tt₂.p1 = mulpt2(tt.p1, bc.y);
				tt₂.p2 = mulpt2(tt.p2, bc.z);

				tp.x = (tt₂.p0.x + tt₂.p1.x + tt₂.p2.x)*Dx(modeltex->r);
				tp.y = (1 - (tt₂.p0.y + tt₂.p1.y + tt₂.p2.y))*Dy(modeltex->r);

				unloadmemimage(modeltex, rectaddpt(Rect(0,0,1,1), tp), cbuf+1, sizeof cbuf - 1);
			}else
				memset(cbuf+1, 0xFF, sizeof cbuf - 1);

			fsp.p = p;
			fsp.bc = bc;
			pixel(params->dst, p, params->fshader(&fsp));
		}
}

void
shaderunit(void *arg)
{
	SUparams *params;
	VSparams vsp;
	Memimage *frag;
	OBJVertex *verts, *tverts, *nverts;	/* geometric, texture and normals vertices */
	OBJIndexArray *idxtab;
	OBJElem **ep;
	Triangle3 t, st, nt;			/* world-, screen-space and normals triangles */
	Triangle2 tt;				/* texture triangle */
	Point3 n;				/* surface normal */
	Point3 np0, np1, bc;
	Triangle2 st₂;

	params = arg;
	vsp.su = params;
	frag = rgb(DBlack);

	threadsetname("shader unit #%d", params->id);

	verts = model->vertdata[OBJVGeometric].verts;
	tverts = model->vertdata[OBJVTexture].verts;
	nverts = model->vertdata[OBJVNormal].verts;

	for(ep = params->b; ep != params->e; ep++){
		idxtab = &(*ep)->indextab[OBJVGeometric];

		t.p0 = Pt3(verts[idxtab->indices[0]].x,verts[idxtab->indices[0]].y,verts[idxtab->indices[0]].z,verts[idxtab->indices[0]].w);
		t.p1 = Pt3(verts[idxtab->indices[1]].x,verts[idxtab->indices[1]].y,verts[idxtab->indices[1]].z,verts[idxtab->indices[1]].w);
		t.p2 = Pt3(verts[idxtab->indices[2]].x,verts[idxtab->indices[2]].y,verts[idxtab->indices[2]].z,verts[idxtab->indices[2]].w);

		idxtab = &(*ep)->indextab[OBJVNormal];
		if(idxtab->nindex == 3){
			nt.p0 = Vec3(nverts[idxtab->indices[0]].i, nverts[idxtab->indices[0]].j, nverts[idxtab->indices[0]].k);
			nt.p1 = Vec3(nverts[idxtab->indices[1]].i, nverts[idxtab->indices[1]].j, nverts[idxtab->indices[1]].k);
			nt.p2 = Vec3(nverts[idxtab->indices[2]].i, nverts[idxtab->indices[2]].j, nverts[idxtab->indices[2]].k);
			nt.p0 = normvec3(nt.p0);
			nt.p1 = normvec3(nt.p1);
			nt.p2 = normvec3(nt.p2);
		}else{
			n = normvec3(crossvec3(subpt3(t.p2, t.p0), subpt3(t.p1, t.p0)));
			nt.p0 = nt.p1 = nt.p2 = mulpt3(n, -1);
		}

		vsp.p = &t.p0;
		vsp.n = &nt.p0;
		vsp.idx = 0;
		st.p0 = params->vshader(&vsp);
		vsp.p = &t.p1;
		vsp.n = &nt.p1;
		vsp.idx = 1;
		st.p1 = params->vshader(&vsp);
		vsp.p = &t.p2;
		vsp.n = &nt.p2;
		vsp.idx = 2;
		st.p2 = params->vshader(&vsp);

		st₂.p0 = Pt2(st.p0.x/st.p0.w, st.p0.y/st.p0.w, 1);
		st₂.p1 = Pt2(st.p1.x/st.p1.w, st.p1.y/st.p1.w, 1);
		st₂.p2 = Pt2(st.p2.x/st.p2.w, st.p2.y/st.p2.w, 1);
		bc = barycoords(st₂, centroid(st₂));
		np0 = centroid3((Triangle3){divpt3(st.p0, st.p0.w),divpt3(st.p1, st.p1.w),divpt3(st.p2, st.p2.w)});
		np1 = Vec3(
			nt.p0.x*bc.x + nt.p1.x*bc.y + nt.p2.x*bc.z,
			nt.p0.y*bc.x + nt.p1.y*bc.y + nt.p2.y*bc.z,
			nt.p0.z*bc.x + nt.p1.z*bc.y + nt.p2.z*bc.z);
		np1 = addpt3(np0, mulpt3(np1, Dx(fb->r)/32));
		triangle(nfb, Pt(st₂.p0.x,st₂.p0.y), Pt(st₂.p1.x,st₂.p1.y), Pt(st₂.p2.x,st₂.p2.y), red);
		bresenham(nfb, Pt(np0.x,np0.y), Pt(np1.x,np1.y), green);

		idxtab = &(*ep)->indextab[OBJVTexture];
		if(modeltex != nil && idxtab->nindex == 3){
			tt.p0 = Pt2(tverts[idxtab->indices[0]].u, tverts[idxtab->indices[0]].v, 1);
			tt.p1 = Pt2(tverts[idxtab->indices[1]].u, tverts[idxtab->indices[1]].v, 1);
			tt.p2 = Pt2(tverts[idxtab->indices[2]].u, tverts[idxtab->indices[2]].v, 1);
		}else
			memset(&tt, 0, sizeof tt);

		rasterize(params, st, tt, frag);
	}

	freememimage(frag);
	sendp(params->donec, nil);
	free(params);
	threadexits(nil);
}

void
shade(Memimage *dst, Shader *s)
{
	int i, nelems, nparts, nworkers;
	uvlong time;
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
	if(nelems < nprocs){
		nworkers = nelems;
		nparts = 1;
	}else{
		nworkers = nprocs;
		nparts = nelems/nprocs;
	}
	time = nanosec();

	donec = chancreate(sizeof(void*), 0);

	for(i = 0; i < nworkers; i++){
		params = emalloc(sizeof *params);
		params->dst = dst;
		params->b = &elems[i*nparts];
		params->e = params->b + nparts;
		params->id = i;
		params->donec = donec;
		params->uni_time = time;
		params->vshader = s->vshader;
		params->fshader = s->fshader;
		proccreate(shaderunit, params, mainstacksize);
//		fprint(2, "spawned su %d for elems [%d, %d)\n", params->id, i*nparts, i*nparts+nparts);
	}

	while(i--)
		recvp(donec);
	chanfree(donec);
}

Point3
ivshader(VSparams *sp)
{
	Matrix3 M, V;

	identity3(M);
	identity3(V);
	mulm3(M, rota);
	mulm3(V, view);
	mulm3(V, M);

	return xform3(*sp->p, V);
}

Memimage *
triangleshader(FSparams *sp)
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
circleshader(FSparams *sp)
{
	Point2 uv;
	double r;
	uchar cbuf[4];

	uv = Pt2(sp->p.x,sp->p.y,1);
	uv.x /= Dx(fb->r);
	uv.y /= Dy(fb->r);
//	r = 0.3;
	r = 0.3*sin(sp->su->uni_time/1e9);

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
sfshader(FSparams *sp)
{
	Point2 uv;
	double y, pct;
	uchar cbuf[4];

	uv = Pt2(sp->p.x,sp->p.y,1);
	uv.x /= Dx(fb->r);
	uv.y /= Dy(fb->r);
	uv.y = 1 - uv.y;		/* make [0 0] the bottom-left corner */

//	y = step(0.5, uv.x);
//	y = pow(uv.x, 5);
//	y = sin(uv.x);
	y = sin(uv.x*sp->su->uni_time/1e8)/2.0 + 0.5;
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
boxshader(FSparams *sp)
{
	Point2 uv, p;
	Point2 r;
	uchar cbuf[4];

	uv = Pt2(sp->p.x,sp->p.y,1);
	uv.x /= Dx(fb->r);
	uv.y /= Dy(fb->r);
	r = Vec2(0.2,0.4);

	p = Pt2(fabs(uv.x - 0.5), fabs(uv.y - 0.5), 1);
	p = subpt2(p, r);
	p.x = fmax(p.x, 0);
	p.y = fmax(p.y, 0);

	if(vec2len(p) > 0)
		return nil;

	cbuf[0] = 0xFF;
	cbuf[1] = 0xFF*smoothstep(0,1,uv.x+uv.y);
	cbuf[2] = 0xFF*uv.y;
	cbuf[3] = 0xFF*uv.x;

	memfillcolor(sp->frag, *(ulong*)cbuf);
	return sp->frag;
}

Shader shadertab[] = {
	{ "triangle", ivshader, triangleshader },
	{ "circle", ivshader, circleshader },
	{ "box", ivshader, boxshader },
	{ "sf", ivshader, sfshader },
	{ "gouraud", vertshader, gouraudshader },
	{ "toon", vertshader, toonshader },
};
Shader *
getshader(char *name)
{
	int i;

	for(i = 0; i < nelem(shadertab); i++)
		if(strcmp(shadertab[i].name, name) == 0)
			return &shadertab[i];
	return nil;
}

void
drawstats(void)
{
	char buf[128];

	/* fps stats hold latency, so max period is min frequency */
	snprint(buf, sizeof buf, "FPS %.0f/%.0f/%.0f/%.0f", !fps.max? 0: 1e9/fps.max, !fps.avg? 0: 1e9/fps.avg, !fps.min? 0: 1e9/fps.min, !fps.v? 0: 1e9/fps.v);
	stringbg(screen, Pt(screen->r.min.x+10,screen->r.max.y-20), display->black, ZP, font, buf, display->white, ZP);
}

void
redraw(void)
{
	lockdisplay(display);
	memfillcolor(screenfb, 0x888888FF);
	memimagedraw(screenfb, screenfb->r, curfb, ZP, nil, ZP, SoverD);
	if(shownormals)
		memimagedraw(screenfb, screenfb->r, nfb, ZP, nil, ZP, SoverD);
	loadimage(screen, rectaddpt(screenfb->r, screen->r.min), byteaddr(screenfb, screenfb->r.min), bytesperline(screenfb->r, screenfb->depth)*Dy(screenfb->r));
	drawstats();
	flushimage(display, 1);
	unlockdisplay(display);

	sendp(donedrawc, nil);
}

void
render(Shader *s)
{
	uvlong t0, t1;

	memsetd(zbuf, Inf(-1), Dx(fb->r)*Dy(fb->r));
	memfillcolor(fb, DTransparent);
	memfillcolor(zfb, DTransparent);
	memfillcolor(nfb, DTransparent);

	t0 = nanosec();
	shade(fb, s);
	t1 = nanosec();
	updatestats(&fps, t1-t0);
}

void
renderer(void *arg)
{
	threadsetname("renderer");

	for(;;){
		rendering = 1;
		render((Shader*)arg);
		rendering = 0;
		nbsendp(drawc, nil);
		recvp(donedrawc);
	}
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
	fprint(2, "usage: %s [-n nprocs] [-m objfile] [-t texfile] [-a yrotangle] [-z camzpos] [-s shader]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	Mousectl *mc;
	Keyboardctl *kc;
	Rune r;
	Shader *s;
	char *mdlpath, *texpath;
	char *sname;
	int fbw, fbh;

	GEOMfmtinstall();
	mdlpath = "mdl/def.obj";
	texpath = nil;
	sname = "gouraud";
	fbw = 200;
	fbh = 200;
	ω = 20*DEG;
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
	case 'a':
		θ = strtod(EARGF(usage()), nil)*DEG;
		break;
	case 'v':
		ω = strtod(EARGF(usage()), nil)*DEG;
		break;
	case 'z':
		camera.z = strtod(EARGF(usage()), nil);
		break;
	case 's':
		sname = EARGF(usage());
		break;
	case 'W':
		fbw = strtoul(EARGF(usage()), nil, 10);
		break;
	case 'H':
		fbh = strtoul(EARGF(usage()), nil, 10);
		break;
	default: usage();
	}ARGEND;
	if(argc != 0)
		usage();

	if(nprocs < 1)
		nprocs = strtoul(getenv("NPROC"), nil, 10);

	if((s = getshader(sname)) == nil)
		sysfatal("couldn't find %s shader", sname);

	if((model = objparse(mdlpath)) == nil)
		sysfatal("objparse: %r");
	if(texpath != nil && (modeltex = readtga(texpath)) == nil)
		sysfatal("readtga: %r");

	{
		int i, nv[OBJNVERT], nf;
		OBJObject *o;
		OBJElem *e;

		nf = 0;
		memset(nv, 0, sizeof nv);
		for(i = 0; i < OBJNVERT; i++) nv[i] += model->vertdata[i].nvert;
		for(i = 0; i < OBJHTSIZE; i++) if((o = model->objtab[i]) != nil)
		for(e = o->child; e != nil; e = e->next) if(e->type == OBJEFace) nf++;
		fprint(2, "v %d vn %d vt %d f %d\n", nv[OBJVGeometric], nv[OBJVNormal], nv[OBJVTexture], nf);
	}

	snprint(winspec, sizeof winspec, "-dx %d -dy %d", fbw, fbh);
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
	zfb = eallocmemimage(fb->r, fb->chan);
	curfb = fb;
	nfb = eallocmemimage(fb->r, fb->chan);
	red = rgb(DRed);
	green = rgb(DGreen);
	blue = rgb(DBlue);

	viewport(fb->r);
	projection(-1.0/vec3len(subpt3(camera, center)));
	identity3(rota);
	lookat(camera, center, up);
	mulm3(view, proj);
	light = normvec3(subpt3(light, center));

	drawc = chancreate(sizeof(void*), 1);
	donedrawc = chancreate(sizeof(void*), 1);
	display->locking = 1;
	unlockdisplay(display);

	proccreate(renderer, s, mainstacksize);

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
			if(!rendering)
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
