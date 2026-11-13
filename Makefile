CC ?= cc
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
ASTERISK_SRC ?=
MODULE_DIR ?= /usr/lib/asterisk/modules
BUILD := build
# CI supplies the Git tag or commit; source builds may override this label.
ADDON_VERSION ?= dev
export ADDON_VERSION
AMR_LIBS := opencore-amrnb opencore-amrwb vo-amrwbenc
PJ_SRC = $(ASTERISK_SRC)/third-party/pjproject/source
AST_CPPFLAGS = -I$(ASTERISK_SRC)/include -Isrc
PJ_CPPFLAGS = -include $(BUILD)/pjproject-config.h $(foreach dir,pjlib pjlib-util pjnath pjmedia pjsip,-I$(PJ_SRC)/$(dir)/include)
WARNINGS := -Wall -Wextra -Werror -Wno-old-style-declaration -Wno-unused-parameter -Wno-ignored-qualifiers
CFLAGS ?= -O2 -g
MODULE_CFLAGS := $(CFLAGS) $(WARNINGS) -fPIC
CODEC_CPPFLAGS := -DAST_MODULE=\"codec_amr\" -DAST_MODULE_SELF_SYM=__internal_codec_amr_self
GUARD_CPPFLAGS := -DAST_MODULE=\"res_pjsip_amr\" -DAST_MODULE_SELF_SYM=__internal_res_pjsip_amr_self

JIO_CPPFLAGS := -DAST_MODULE=\"res_pjsip_jio\" -DAST_MODULE_SELF_SYM=__internal_res_pjsip_jio_self
MODULES := $(BUILD)/codec_amr.so $(BUILD)/res_pjsip_amr.so $(BUILD)/res_pjsip_jio.so

.PHONY: all check-sdk install clean
all: $(MODULES)

$(BUILD):
	mkdir -p $@

check-sdk: | $(BUILD)
	$(PYTHON) scripts/check_sdk.py "$(ASTERISK_SRC)" --manifest $(BUILD)/build-info.json --header $(BUILD)/pjproject-config.h

$(BUILD)/codec_amr.so: src/codec_amr.c src/amr_format.c src/amr.h src/amr_payload.h src/ex_amr.h | check-sdk
	$(CC) $(CPPFLAGS) $(AST_CPPFLAGS) $(CODEC_CPPFLAGS) $(MODULE_CFLAGS) \
		$$($(PKG_CONFIG) --cflags $(AMR_LIBS)) -shared src/codec_amr.c src/amr_format.c \
		-o $@ $(LDFLAGS) $$($(PKG_CONFIG) --libs $(AMR_LIBS))

$(BUILD)/res_pjsip_amr.so: src/res_pjsip_amr.c src/amr.h | check-sdk
	$(CC) $(CPPFLAGS) $(AST_CPPFLAGS) $(PJ_CPPFLAGS) $(GUARD_CPPFLAGS) $(MODULE_CFLAGS) \
		-shared $< -o $@ $(LDFLAGS)

$(BUILD)/res_pjsip_jio.so: src/jio/res_pjsip_jio.c src/jio/jio_contact.h | check-sdk
	$(CC) $(CPPFLAGS) $(AST_CPPFLAGS) $(PJ_CPPFLAGS) $(JIO_CPPFLAGS) $(MODULE_CFLAGS) \
		-shared $< -o $@ $(LDFLAGS)

install: all
	install -d "$(DESTDIR)$(MODULE_DIR)"
	install -m 755 $(MODULES) "$(DESTDIR)$(MODULE_DIR)/"

clean:
	rm -rf $(BUILD)
