</$objtype/mkfile

BIN=/$objtype/bin
TARG=tinyrend
OFILES=\
	main.$O\
	nanosec.$O\

LIB=\
	libobj/libobj.a$O\

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
