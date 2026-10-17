CC ?= cc
PYTHON ?= python3
CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror

ifeq ($(OS),Windows_NT)
LIB := codex-chronos.dll
LDFLAGS := -shared -static-libgcc -lws2_32
else
CFLAGS += -fPIC -fstack-protector-strong
ifeq ($(shell uname -s),Darwin)
LIB := codex-chronos.dylib
LDFLAGS := -dynamiclib -pthread
# Some new SDKs list arm64e slices that an older ld64 cannot parse. Try the
# default toolchain first; on failure retry with each distinct installed SDK,
# newest first. Resolve symlinks so aliases such as MacOSX.sdk and MacOSX27.sdk
# do not make the same broken SDK get retried repeatedly. No effect on healthy
# toolchains.
DARWIN_SDKS := $(shell { for sdk in /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk; do [ -d "$$sdk" ] && (cd "$$sdk" && pwd -P); done; } 2>/dev/null | sort -r -u)
else
LIB := codex-chronos.so
LDFLAGS := -pthread -shared
endif
endif

all: $(LIB)
dashboard_bundle.h: dashboard.html embed_ui.py
	$(PYTHON) embed_ui.py
$(LIB): plugin.c settings.inc events.inc stats.inc refresh_bridge.inc failures.inc timezone_normalize.inc model_monitor.inc paths.inc spawn_refresh.inc platform_compat.h dashboard_bundle.h
ifdef DARWIN_SDKS
	@ok=0; \
	if $(CC) $(CFLAGS) -o $@ $< $(LDFLAGS); then ok=1; fi; \
	if [ $$ok -eq 0 ]; then \
	  for sdk in $(DARWIN_SDKS); do \
	    echo "default SDK link failed; retrying with $$sdk"; \
	    if $(CC) $(CFLAGS) --sysroot=$$sdk -o $@ $< $(LDFLAGS); then ok=1; break; fi; \
	  done; \
	fi; \
	if [ $$ok -eq 0 ]; then echo "link failed for every installed SDK"; exit 1; fi
else
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
endif
test: all
	$(PYTHON) test_plugin.py
clean:
	rm -f codex-chronos.so codex-chronos.dylib codex-chronos.dll dashboard_bundle.h
.PHONY: all clean test

# Optional browser tests require Playwright and Chromium.
test-ui:
	$(PYTHON) test_ui.py
.PHONY: test-ui
