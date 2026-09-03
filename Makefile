APXS ?= apxs
SRC   = src/mod_cacher.c src/cacher_config.c src/cacher_rules.c src/cacher_cache.c src/cacher_util.c third_party/cJSON.c
INC   = -Ithird_party

# Compile only. Touches nothing outside this directory - safe on a live server.
all:
	$(APXS) -c $(INC) $(SRC)

# Copy the built module into Apache's modules dir. Does NOT edit any Apache
# config and does NOT load the module - add the LoadModule line yourself when
# you are ready. Nothing changes for a running Apache until you do.
install:
	$(APXS) -i $(INC) $(SRC)

# Install AND add the LoadModule line to the main Apache config (apxs -a).
# This edits httpd.conf. Back it up first and run `apachectl configtest`
# before restarting.
enable:
	$(APXS) -i -a $(INC) $(SRC)

test:
	$(CC) -Isrc -Ithird_party -o test/test_rules_parse \
		test/test_rules_parse.c src/cacher_rules.c third_party/cJSON.c
	./test/test_rules_parse

clean:
	rm -rf src/*.o src/*.lo src/*.slo src/.libs .libs *.o *.lo *.slo test/test_rules_parse

.PHONY: all install enable test clean
