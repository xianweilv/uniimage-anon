CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
CFLAGS  += -MMD -MP
LDFLAGS ?=
LIBS     = -lz -lcrypto -lm

SRC_HD = hd_common.c hd_huffman.c hd_tokdec.c hd_tokenc.c hd_lz77.c \
         hd_fastcdc.c hd_store.c hd_recipe.c hd_ingest.c hd_synth.c \
         hd_splice.c
OBJ_HD = $(SRC_HD:.c=.o)

TOOLS_SRC = $(filter-out tools/ld_verify.c,$(wildcard tools/*.c))
TOOLS = $(TOOLS_SRC:.c=)
#   gcc -O2 -o tools/ld_verify tools/ld_verify.c ~/libdeflate/libdeflate.a -I~/libdeflate -lcrypto

all: $(TOOLS)

libhd.a: $(OBJ_HD)
	ar rcs $@ $^

tools/%: tools/%.o libhd.a
	$(CC) $(CFLAGS) -o $@ $< libhd.a $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(OBJ_HD:.o=.d) $(TOOLS_SRC:.c=.d)

clean:
	rm -f $(OBJ_HD) tools/*.o $(TOOLS) libhd.a *.d tools/*.d

.PHONY: all clean
