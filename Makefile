CC=gcc
CFLAGS=-g -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE -Wmissing-prototypes
OFILES=$(filter-out main%.o,$(addsuffix .o,$(basename $(wildcard *.c))))
HFILES=$(wildcard *.h)
EXES=agent client tcfreg valueadd tcflog
UNAME=$(shell uname -o)

ifeq ($(UNAME),Cygwin)
LIBS=-lws2_32 -liphlpapi
else
ifeq ($(UNAME),Msys)
CFLAGS:=-mwin32 $(CFLAGS)
LIBS=-lws2_32 -liphlpapi -limagehlp
else
LIBS=-lpthread -lrt -lelf
endif
endif

all:	$(EXES)

agent: main.o $(OFILES)
	$(CC) $(CFLAGS) -o $@ main.o $(OFILES) $(LIBS)

client: main_client.o $(OFILES)
	$(CC) $(CFLAGS) -o $@ main_client.o $(OFILES) $(LIBS)

tcfreg: main_reg.o $(OFILES)
	$(CC) $(CFLAGS) -o $@ main_reg.o $(OFILES) $(LIBS)

valueadd: main_va.o $(OFILES)
	$(CC) $(CFLAGS) -o $@ main_va.o $(OFILES) $(LIBS)

tcflog: main_log.o $(OFILES)
	$(CC) $(CFLAGS) -o $@ main_log.o $(OFILES) $(LIBS)

%.o: %.c $(HFILES)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o *.exe $(EXES)
