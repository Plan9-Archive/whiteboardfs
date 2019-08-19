</$objtype/mkfile

TARG=whiteboardfs dryerase fill

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

all:V: $EXE

install:V: $INST

clean:V:
	rm -f $O.$TARG [$OS].$TARG *.[$OS]
