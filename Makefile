CC = gcc
LD = $(CC)

CFLAGS  = -fPIC
LDFLAGS = -shared
SHLIB = metrofix.so

OBJ = dynapi/SDL_dynapi.o -lm

.SUFFIXES:
.SUFFIXES: .o .c

all: $(SHLIB)

$(SHLIB): $(OBJ)
	$(LD) -o $@ $(LDFLAGS) $(OBJ) $(LDLIBS)

.c.o:
	$(CC) $(CFLAGS) -o $@ -c $<

distclean: clean
	$(RM) *.so*
clean:
	$(RM) *.o dynapi/*.o
