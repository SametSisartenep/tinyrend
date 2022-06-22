</$objtype/mkfile

BIN=/$objtype/bin/musw
TARG=tinyrend
OFILES=\
	main.$O\

HFILES=\
	./libgeometry/geometry.h\

LIB=\
	./libgeometry/libgeometry.a$O\

CFLAGS=$CFLAGS

</sys/src/cmd/mkone

./libgeometry/libgeometry.a$O:
	cd ./libgeometry
	mk install

clean nuke:V:
	rm -f *.[$OS] [$OS].??* $TARG
	@{cd ./libgeometry; mk $target}
