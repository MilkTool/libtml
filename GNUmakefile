IDIRS   := -I ten-lang/src/
LDIRS   := -L ten-lang/
CCFLAGS := -Wall
PROFILE ?= release

PREFIX ?= /usr/local/
LIBDIR ?= $(shell if [ -d $(PREFIX)/lib64 ]; then echo $(PREFIX)/lib64; else echo $(PREFIX)/lib; fi )
INCDIR ?= $(PREFIX)/include

ifeq ($(PROFILE),release)
    CCFLAGS += -O2 -D NDEBUG
    POSTFIX := 
else
    ifeq ($(PROFILE),debug)
        CCFLAGS += -g -O0
        POSTFIX := -debug
    else
        $(error "Invalid build profile")
    endif
endif

libtml$(POSTFIX).a: tml.h tml.c
	$(CC) $(CCFLAGS) $(IDIRS) $(LDIRS) -c tml.c -o tml.o
	ar rcs libtml$(POSTFIX).a tml.o

.PHONY: install
install:
	mkdir -p $(LIBDIR)
	cp libtml$(POSTFIX).a $(LIBDIR)
	cp tml.h $(INCDIR)

.PHONY: clean
clean:
	- rm *.a
	- rm *.o
