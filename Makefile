CC = gcc
LD = $(CC)

CFLAGS  = -fPIC -Os
LDFLAGS = -shared
SHLIB = metrofix.so

# List your headers here or use a wildcard
HEADERS = $(wildcard dynapi/*.h)
OBJ = dynapi/SDL_dynapi.o

# The math library (-lm) should be in LDLIBS, not OBJ
LDLIBS =

.SUFFIXES:
.SUFFIXES: .o .c

all: $(SHLIB)

$(SHLIB): $(OBJ)
	$(LD) -o $@ $(LDFLAGS) $(OBJ) $(LDLIBS)

# This line tells make that every .o file depends on the .h files
$(OBJ): $(HEADERS)

.c.o:
	$(CC) $(CFLAGS) -o $@ -c $<

distclean: clean
	$(RM) $(SHLIB)
clean:
	$(RM) *.o dynapi/*.o
