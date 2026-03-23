CC = gcc
LD = $(CC)

CFLAGS  = -fPIC -Os
LDFLAGS = -shared -s
SHLIB = metrofix.so

HEADERS = $(wildcard dynapi/*.h)
OBJ = dynapi/SDL_dynapi.o

LDLIBS =

.SUFFIXES:
.SUFFIXES: .o .c

all: $(SHLIB)

$(SHLIB): $(OBJ)
	$(LD) -o $@ $(LDFLAGS) $(OBJ) $(LDLIBS)

$(OBJ): $(HEADERS)

.c.o:
	$(CC) $(CFLAGS) -o $@ -c $<

distclean: clean
	$(RM) $(SHLIB)
clean:
	$(RM) *.o dynapi/*.o
