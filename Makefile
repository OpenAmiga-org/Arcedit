# arcedit - edit archives without unpacking them
#
#   make            host build (bin/arcedit, used by the tests)
#   make test       run the host tests (needs lha, LHa for UNIX)
#   make os4        AmigaOS 4 build in the walkero docker (bin/os4/arcedit)

SRC = src/main.c src/lha.c src/lh5enc.c src/lhaformat.c src/deflate.c src/zipformat.c src/tarformat.c src/gzformat.c src/rarformat.c src/lzxformat.c src/platform.c
HDR = src/arcformat.h src/lha.h src/deflate.h src/tar.h src/platform.h

CFLAGS = -O2 -Wall -Wextra -std=gnu99

all: bin/arcedit

bin/arcedit: $(SRC) $(HDR)
	mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: bin/arcedit
	sh tests/run.sh

os4:
	mkdir -p bin/os4
	sudo docker run --rm -u $$(id -u):$$(id -g) -v $(CURDIR):/src -w /src \
	  walkero/amigagccondocker:os4-gcc15-3.6.1 \
	  ppc-amigaos-gcc -O2 -Wall -Wextra -std=gnu99 -o bin/os4/arcedit $(SRC)

clean:
	rm -f bin/arcedit bin/os4/arcedit

.PHONY: all test os4 clean
