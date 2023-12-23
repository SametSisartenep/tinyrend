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
