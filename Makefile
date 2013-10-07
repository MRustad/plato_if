
SIMPLE_TARGETS := plato_if

OBJ := obj

__cflags = -O2 -Wall -Werror -Wextra -Wmissing-prototypes -Wstrict-prototypes \
	-Wcast-align -Wcast-qual -Wformat=2 -Wundef -MMD -MF ${OBJ}/$@.d -g
__ldflags = -O2 -Wall -Werror -g

#LIBS_plato_if := -lbcm2835 -lrt -lasound
LIBS_plato_if := -lrt -lasound

all: ${OBJ} ${TARGETS} ${SIMPLE_TARGETS}

-include $(wildcard ${OBJ}/*.d)

.SUFFIXES:

${SIMPLE_TARGETS}: ${OBJ}
	${CC} ${__cflags} ${CFLAGS} -o $@ $@.c ${LIBS_$@}

%:	${OBJ}/%.o ${OBJ}
	${CC} ${__ldflags} ${CFLAGS} -o $@ $< ${LIBS_$@}

${OBJ}/%: ${OBJ}

${OBJ}/%.o: %.c ${OBJ}
	${CC} ${__cflags} ${CFLAGS} -c -o $@ $<

${OBJ}:
	mkdir -p $@

.PHONY:	clean
clean:
	rm -f ${TARGETS} ${SIMPLE_TARGETS}
	rm -rf ${OBJ}
