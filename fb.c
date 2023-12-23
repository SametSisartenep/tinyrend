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

extern int shownormals;	/* XXX DBG */

static void
framebufctl_draw(Framebufctl *ctl, Memimage *dst, int showz)
{
	lock(&ctl->swplk);
	memimagedraw(dst, dst->r, showz? ctl->fb[ctl->idx]->zb: ctl->fb[ctl->idx]->cb, ZP, nil, ZP, SoverD);
	/* XXX DBG */
	if(shownormals)
		memimagedraw(dst, dst->r, ctl->fb[ctl->idx]->nb, ZP, nil, ZP, SoverD);
	unlock(&ctl->swplk);
}

static void
framebufctl_swap(Framebufctl *ctl)
{
	lock(&ctl->swplk);
	ctl->idx ^= 1;
	unlock(&ctl->swplk);
}

static void
framebufctl_reset(Framebufctl *ctl)
{
	Framebuf *fb;

	/* address the back buffer—resetting the front buffer is VERBOTEN */
	fb = ctl->fb[ctl->idx^1];
	memsetd(fb->zbuf, Inf(-1), Dx(fb->r)*Dy(fb->r));
	memfillcolor(fb->cb, DTransparent);
	memfillcolor(fb->zb, DTransparent);
	memfillcolor(fb->nb, DTransparent);	/* XXX DBG */
}

Framebuf *
mkfb(Rectangle r)
{
	Framebuf *fb;

	fb = emalloc(sizeof *fb);
	fb->cb = eallocmemimage(r, RGBA32);
	fb->zb = eallocmemimage(r, RGBA32);
	fb->zbuf = emalloc(Dx(r)*Dy(r)*sizeof(*fb->zbuf));
	memsetd(fb->zbuf, Inf(-1), Dx(r)*Dy(r));
	memset(&fb->zbuflk, 0, sizeof(fb->zbuflk));
	fb->nb = eallocmemimage(r, RGBA32);	/* XXX DBG */
	fb->r = r;
	return fb;
}

Framebufctl *
newfbctl(Rectangle r)
{
	Framebufctl *fc;

	fc = emalloc(sizeof *fc);
	memset(fc, 0, sizeof *fc);
	fc->fb[0] = mkfb(r);
	fc->fb[1] = mkfb(r);
	fc->draw = framebufctl_draw;
	fc->swap = framebufctl_swap;
	fc->reset = framebufctl_reset;
	return fc;
}
