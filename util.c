#include <u.h>
#include <libc.h>
#include <thread.h>
#include <draw.h>
#include <memdraw.h>
#include <mouse.h>
#include <keyboard.h>
#include <geometry.h>
#include "libobj/obj.h"
#include "dat.h"
#include "fns.h"

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
memsetd(double *p, double v, usize len)
{
	double *dp;

	for(dp = p; dp < p+len; dp++)
		*dp = v;
}

typedef struct Deco Deco;
struct Deco
{
	int pfd[2];
	int infd;
	char *prog;
};

static void
decproc(void *arg)
{
	char buf[32];
	Deco *d;

	d = arg;

	close(d->pfd[0]);
	dup(d->infd, 0);
	close(d->infd);
	dup(d->pfd[1], 1);
	close(d->pfd[1]);

	snprint(buf, sizeof buf, "/bin/%s", d->prog);

	execl(buf, d->prog, "-9t", nil);
	threadexitsall("execl: %r");
}

static Memimage *
genreadimage(char *prog, char *path)
{
	Memimage *i;
	Deco d;

	d.prog = prog;

	if(pipe(d.pfd) < 0)
		sysfatal("pipe: %r");
	d.infd = open(path, OREAD);
	if(d.infd < 0)
		sysfatal("open: %r");
	procrfork(decproc, &d, mainstacksize, RFFDG|RFNAMEG|RFNOTEG);
	close(d.pfd[1]);
	i = readmemimage(d.pfd[0]);
	close(d.pfd[0]);
	close(d.infd);

	return i;
}

Memimage *
readtga(char *path)
{
	return genreadimage("tga", path);
}

Memimage *
readpng(char *path)
{
	return genreadimage("png", path);
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
