#define HZ2MS(hz)	(1000/(hz))

void resized(void);

/* nanosec */
uvlong nanosec(void);

/* alloc */
void *emalloc(ulong);
void *erealloc(void*, ulong);
Image *eallocimage(Display*, Rectangle, ulong, int, ulong);
Memimage *eallocmemimage(Rectangle, ulong);

/* fb */
Framebuf *mkfb(Rectangle);
Framebufctl *newfbctl(Rectangle);

/* shadeop */
double step(double, double);
double smoothstep(double, double, double);

/* util */
int min(int, int);
int max(int, int);
double fmin(double, double);
double fmax(double, double);
void swap(int*, int*);
void memsetd(double*, double, usize);
Memimage *readtga(char*);
Memimage *readpng(char*);
Memimage *rgb(ulong);
