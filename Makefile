
SIMPLE_TARGETS := plato_if platomsg
INST_DIR := /usr/local/bin

OBJ := obj

__cflags = -O2 -Wall -Werror -Wextra -Wmissing-prototypes -Wstrict-prototypes \
	-Wcast-align -Wcast-qual -Wformat=2 -Wundef -MMD -MF ${OBJ}/$@.d -g
__ldflags = -O2 -Wall -Werror -g

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

.PHONY: install
install: plato_if platomsg
	install -o root -g root $< platod platomsg ${INST_DIR}
	install -o root -g root platod.init /etc/init.d/platod
	install -o root -g root gpio.init /etc/init.d/gpio
	chkconfig platod || chkconfig --add platod
	rm -f /etc/rcS.d/S[0-9][0-9]gpio
	ln -s /etc/init.d/gpio /etc/rcS.d/S36gpio

.PHONY:	clean
clean:
	rm -f ${TARGETS} ${SIMPLE_TARGETS}
	rm -rf ${OBJ}
