############################################################################
# SPDX-License-Identifier: Apache-2.0
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = anki_import
PRIORITY  = $(CONFIG_MOXINZHI_ANKI_IMPORTER_PRIORITY)
STACKSIZE = $(CONFIG_MOXINZHI_ANKI_IMPORTER_STACKSIZE)
MODULE    = $(CONFIG_MOXINZHI_ANKI_IMPORTER)

MAINSRC = anki_import_main.c

CSRCS += anki_importer/anki_importer.c
CSRCS += anki_importer/anki_sha256.c
CSRCS += anki_importer/anki_text.c
CSRCS += card_model/card_model.c

CFLAGS += ${INCDIR_PREFIX}include
CFLAGS += ${INCDIR_PREFIX}anki_importer
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/system/zlib/zlib
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/sqlite/build
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../external/unqlite/unqlite

include $(APPDIR)/Application.mk
