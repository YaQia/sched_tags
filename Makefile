# sched_tag — lightweight source-level scheduling tags.
#
# The LLVM pass toolchain that used to live in this repository is preserved on
# the `deprecated/pass` branch. master only builds the userspace library and
# its token-API unit test.
#
# If <linux/sched_hint.h> is not in the default system include path, point at
# the kernel's installed UAPI headers:
#   make KERNEL_HEADERS=/path/to/kernel/usr/include
#   make check KERNEL_HEADERS=/path/to/kernel/usr/include

CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g -Wall -Wextra -fPIC
CPPFLAGS ?=
LDFLAGS ?=

KERNEL_HEADERS ?=
INCLUDES := -Iinclude
ifneq ($(strip $(KERNEL_HEADERS)),)
INCLUDES += -I$(KERNEL_HEADERS)
endif

LIB := libsched_tag.a
OBJ := sched_tag.o
TEST := test/reader_tokens

.PHONY: all lib check test clean

all: lib

lib: $(LIB)

$(OBJ): sched_tag.c include/sched_tag.h include/sched_hint.h
	$(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

$(TEST): test/reader_tokens.c include/sched_tag.h include/sched_hint.h
	$(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) $< $(LDFLAGS) -o $@

check: $(TEST)
	./$(TEST)

test: check

clean:
	rm -f $(OBJ) $(LIB) $(TEST)
