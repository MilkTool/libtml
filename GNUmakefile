CCFLAGS := -Wall -O2 -g
PREFIX ?= /usr/local/
LIBDIR ?= $(shell if [ -d $(PREFIX)/lib64 ]; then echo $(PREFIX)/lib64; else echo $(PREFIX)/lib; fi )
INCDIR ?= $(PREFIX)/include

libtml.a: tml.h tml.c
	$(CC) $(CCFLAGS) -c tml.c -o tml.o
	ar rcs libtml.a tml.o

.PHONY: install
install:
	mkdir -p $(LIBDIR)
	mkdir -p $(INCDIR)
	cp libtml.a $(LIBDIR)
	cp tml.h $(INCDIR)

.PHONY: clean
clean:
	- rm *.a
	- rm *.o
