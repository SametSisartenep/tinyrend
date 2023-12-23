typedef Point Triangle[3];
typedef struct VSparams VSparams;
typedef struct FSparams FSparams;
typedef struct SUparams SUparams;
typedef struct Shader Shader;
typedef struct Framebuf Framebuf;
typedef struct Framebufctl Framebufctl;

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
	Framebuf *fb;
	OBJElem **b, **e;
	int id;
	Channel *donec;

	double var_intensity[3];

	uvlong uni_time;

	Point3 (*vshader)(VSparams*);
	Memimage *(*fshader)(FSparams*);
};

struct Shader
{
	char *name;
	Point3 (*vshader)(VSparams*);
	Memimage *(*fshader)(FSparams*);
};

struct Framebuf
{
	Memimage *cb;
	Memimage *zb;
	double *zbuf;
	Lock zbuflk;
	Memimage *nb;	/* XXX DBG */
	Rectangle r;
};

struct Framebufctl
{
	Framebuf *fb[2];
	uint idx;
	Lock swplk;

	void (*draw)(Framebufctl*, Memimage*, int);
	void (*swap)(Framebufctl*);
	void (*reset)(Framebufctl*);
};

typedef struct Stats Stats;
struct Stats
{
	uvlong min, avg, max, acc, n, v;
};
