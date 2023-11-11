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

clean nuke:V:
	rm -f *.[$OS] [$OS].out $TARG
	@{cd libobj; mk $target}
