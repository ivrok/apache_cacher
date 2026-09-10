APXS ?= apxs
SRC   = src/mod_cacher.c src/cacher_config.c src/cacher_rules.c \
        src/cacher_cache.c src/cacher_util.c src/cacher_admin.c \
        third_party/cJSON.c
INC   = -Ithird_party

# An implicit declaration in C means the compiler assumes int, which
# silently truncates a returned pointer to 32 bits on a 64-bit build. That
# shipped once already (apr_psprintf without apr_strings.h, producing a
# corrupt Age header and a crashing worker), so it is an error here.
SAFETY = -Wc,-Werror=implicit-function-declaration

MODULE = src/mod_cacher.la
NAME   = cacher
BINDIR ?= /usr/local/bin

# Compile only. Touches nothing outside this directory - safe on a live server.
all: $(MODULE)

$(MODULE): $(SRC)
	$(APXS) -c $(SAFETY) $(INC) $(SRC)

# Copy the built module into Apache's modules dir. Does NOT edit any Apache
# config and does NOT load the module - add the LoadModule line yourself when
# you are ready. Nothing changes for a running Apache until you do.
#
# Note: apxs -i takes the built .la archive, NOT the source list. Passing
# sources here makes it copy the first .c file into the modules directory
# and then fail looking for a .so that was never installed.
install: $(MODULE)
	$(APXS) -i -n $(NAME) $(MODULE)
	install -m 0755 tools/cacher $(BINDIR)/cacher
	@echo "installed $(BINDIR)/cacher (list / purge / full-reset)"

# Install AND add the LoadModule line to the main Apache config (apxs -a).
# This edits the config. Back it up first and run `apachectl configtest`
# before restarting.
enable: $(MODULE)
	$(APXS) -i -a -n $(NAME) $(MODULE)

# Rebuild with warnings on, showing only our own code (cJSON is upstream).
# Should print nothing - anything it prints is worth reading.
warn:
	@$(MAKE) clean >/dev/null
	@$(APXS) -c -Wc,-Wall -Wc,-Wextra $(SAFETY) $(INC) $(SRC) 2>&1 \
		| grep -E 'warning:|error:' | grep -v third_party || echo "No warnings."

test:
	$(CC) -Isrc -Ithird_party -o test/test_rules_parse \
		test/test_rules_parse.c src/cacher_rules.c third_party/cJSON.c
	./test/test_rules_parse

clean:
	rm -rf src/*.o src/*.lo src/*.slo src/*.la src/.libs \
		third_party/*.o third_party/*.lo third_party/*.slo third_party/.libs \
		.libs *.o *.lo *.slo test/test_rules_parse

.PHONY: all install enable warn test clean
