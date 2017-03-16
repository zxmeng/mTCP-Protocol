CC=gcc
CFLAGS=-Wall -I.
LDFLAGS=-lpthread

EXE = server

OBJ = server.o mtcp_server.o

${EXE}: ${OBJ}
	${CC} ${CFLAGS} -o ${EXE} ${OBJ} ${LDFLAGS}

clean:
	rm -f ${EXE} ${OBJ}
