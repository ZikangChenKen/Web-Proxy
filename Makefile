COURSE = /clear/www/htdocs/comp321

CC = cc
CFLAGS = -std=gnu11 -Wall -Wextra -Werror -I${COURSE}/include -g
LDFLAGS = -lpthread -lnsl -lrt -lresolv

PROG = proxy
OBJS = proxy.o csapp.o

all: proxy

proxy: ${OBJS}
	${CC} ${CFLAGS} -o ${PROG} ${OBJS} ${LDFLAGS}

proxy.o: proxy.c ${COURSE}/include/csapp.h
	${CC} ${CFLAGS} -c proxy.c

csapp.o: ${COURSE}/src/csapp.c ${COURSE}/include/csapp.h
	${CC} ${CFLAGS} -c ${COURSE}/src/csapp.c -o csapp.o

clean:
	${RM} *.o proxy core.[1-9]*

.PHONY: all clean
