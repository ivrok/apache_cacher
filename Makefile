APXS ?= apxs
SRC   = src/mod_cacher.c src/cacher_config.c src/cacher_rules.c src/cacher_cache.c src/cacher_util.c third_party/cJSON.c
INC   = -Ithird_party

all:
	$(APXS) -c $(INC) $(SRC)

install:
	$(APXS) -i -a $(INC) $(SRC)

test:
	$(CC) -Isrc -Ithird_party -o test/test_rules_parse \
		test/test_rules_parse.c src/cacher_rules.c third_party/cJSON.c
	./test/test_rules_parse

clean:
	rm -rf src/*.o src/*.lo src/*.slo src/.libs .libs *.o *.lo *.slo test/test_rules_parse

.PHONY: all install test clean
