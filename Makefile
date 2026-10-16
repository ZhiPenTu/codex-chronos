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
# Some macOS SDKs (27+) list arm64e slices that older ld64 cannot parse.
# Try the default SDK first; on failure retry with older installed SDKs,
# newest first. No effect on healthy toolchains.
DARWIN_SDKS := $(shell ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk 2>/dev/null | sort -r)
else
LIB := codex-chronos.so
LDFLAGS := -pthread -shared
endif
endif

all: $(LIB)
dashboard_bundle.h: dashboard.html embed_ui.py
	$(PYTHON) embed_ui.py
$(LIB): plugin.c settings.inc events.inc stats.inc refresh_bridge.inc failures.inc timezone_normalize.inc model_monitor.inc paths.inc platform_compat.h dashboard_bundle.h
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
