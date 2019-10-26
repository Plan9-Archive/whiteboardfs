</$objtype/mkfile

TARG=whiteboardfs dryerase

EXE=${TARG:%=$O.%}
INST=${TARG:%=/$objtype/bin/%}

default:V: all

/$objtype/bin/%: $O.%
	cp $prereq $target

$O.%: %.$O
	$LD $LDFLAGS -o $target $prereq

$O.whiteboardfs: whiteboardfs.$O imageload.$O
	$LD $LDFLAGS -o $target $prereq

%.$O: %.c
	$CC $CFLAGS $stem.c

/sys/man/4/whiteboardfs: whiteboardfs.4.man
	cp whiteboardfs.4.man /sys/man/4/whiteboardfs

all:V: $EXE

install:V: $INST

man:V: /sys/man/4/whiteboardfs

clean:V:
	rm -f $O.$TARG [$OS].$TARG *.[$OS]
