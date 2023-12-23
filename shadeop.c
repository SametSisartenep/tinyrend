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
