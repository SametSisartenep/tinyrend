</$objtype/mkfile

BIN=/$objtype/bin
TARG=tinyrend
OFILES=\
	main.$O\
	nanosec.$O\
	alloc.$O\
	fb.$O\
	shadeop.$O\
	util.$O\

LIB=\
	libobj/libobj.a$O\

HFILES=dat.h fns.h

</sys/src/cmd/mkone

libobj/libobj.a$O:
	cd libobj
	mk install

pulldeps:VQ:
	git/clone git://antares-labs.eu/libobj || \
	git/clone git://shithub.us/rodri/libobj || \
	git/clone https://github.com/sametsisartenep/libobj

clean nuke:V:
	rm -f *.[$OS] [$OS].out $TARG
	@{cd libobj; mk $target}
