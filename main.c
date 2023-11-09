#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#include <draw.h>
#include <memdraw.h>
#include <mouse.h>
#include <keyboard.h>
#include <geometry.h>


Memimage *fb;
Memimage *red;

void resized(void);

void*
emalloc(ulong n)
{
	void *p;

	p = malloc(n);
	if(p == nil)
		sysfatal("malloc: %r");
	setmalloctag(p, getcallerpc(&n));
	return p;
}

void*
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

Image*
eallocimage(Display *d, Rectangle r, ulong chan, int repl, ulong col)
{
	Image *i;

	i = allocimage(d, r, chan, repl, col);
	if(i == nil)
		sysfatal("allocimage: %r");
	return i;
}

Memimage*
eallocmemimage(Rectangle r, ulong chan)
{
	Memimage *i;

	i = allocmemimage(r, chan);
	if(i == nil)
		sysfatal("allocmemimage: %r");
	memfillcolor(i, DTransparent);
	return i;
}

Memimage*
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
	Rectangle r;

	r.min = addpt(dst->r.min, p);
	r.max = addpt(r.min, Pt(1,1));

	memimagedraw(dst, r, src, ZP, nil, ZP, SoverD);
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
	int steep = 0;
	double t;
	Point p, dp;

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

	for(p = p0; p.x <= p1.x; p.x++){
		t = (double)(p.x-p0.x)/(p1.x-p0.x);
		p.y = flerp(p0.y, p1.y, t);

		if(steep)
			swap(&p.x, &p.y);

		pixel(dst, p, src);

		if(steep)
			swap(&p.x, &p.y);
	}
}

void
redraw(void)
{
	lockdisplay(display);
	draw(screen, screen->r, display->black, nil, ZP);
	loadimage(screen, screen->r, byteaddr(fb, fb->r.min), bytesperline(fb->r, fb->depth)*Dy(fb->r));
	flushimage(display, 1);
	unlockdisplay(display);
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
	fprint(2, "usage: %s\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char *argv[])
{
	Mousectl *mc;
	Keyboardctl *kc;
	Rune r;

	GEOMfmtinstall();
	ARGBEGIN{
	default: usage();
	}ARGEND;
	if(argc > 0)
		usage();

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

	fb = eallocmemimage(screen->r, screen->chan);
	red = rgb(DRed);
	bresenham(fb, Pt(40,40), Pt(300,300), red);
	bresenham(fb, Pt(80,80), Pt(100,200), red);
	bresenham(fb, Pt(80,80), Pt(200,100), red);

	display->locking = 1;
	unlockdisplay(display);
	redraw();

	for(;;){
		enum { MOUSE, RESIZE, KEYBOARD };
		Alt a[] = {
			{mc->c, &mc->Mouse, CHANRCV},
			{mc->resizec, nil, CHANRCV},
			{kc->c, &r, CHANRCV},
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
		}

		redraw();
	}
}

void
resized(void)
{
	lockdisplay(display);
	if(getwindow(display, Refnone) < 0)
		sysfatal("couldn't resize");
	unlockdisplay(display);
	redraw();
}
