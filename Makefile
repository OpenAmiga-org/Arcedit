# arcedit - edit archives without unpacking them
#
#   make            host build (bin/arcedit, used by the tests)
#   make test       run the host tests (needs lha, LHa for UNIX)
#   make os4        AmigaOS 4 build in the walkero docker (bin/os4/arcedit)
#   make fuzz       damaged archives against a sanitizer build (tests/fuzz.py)

SRC = src/main.c src/lha.c src/lh5enc.c src/lhaformat.c src/deflate.c src/zipformat.c src/tarformat.c src/gzformat.c src/lzxformat.c src/platform.c
HDR = src/arcformat.h src/lha.h src/deflate.h src/tar.h src/platform.h

CFLAGS = -O2 -Wall -Wextra -std=gnu99

all: bin/arcedit

bin/arcedit: $(SRC) $(HDR)
	mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: bin/arcedit
	sh tests/run.sh

# AddressSanitizer + UBSan build for the fuzz test (tests/fuzz.py)
bin/arcedit-asan: $(SRC) $(HDR)
	mkdir -p bin
	$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -std=gnu99 -o $@ $(SRC)

asan: bin/arcedit-asan

fuzz: bin/arcedit-asan
	python3 tests/fuzz.py 20000

os4:
	mkdir -p bin/os4
	sudo docker run --rm -u $$(id -u):$$(id -g) -v $(CURDIR):/src -w /src \
	  walkero/amigagccondocker:os4-gcc15-3.6.1 \
	  ppc-amigaos-gcc -O2 -Wall -Wextra -std=gnu99 -o bin/os4/arcedit $(SRC)

clean:
	rm -f bin/arcedit bin/arcedit-asan bin/os4/arcedit

.PHONY: all test os4 clean asan fuzz
