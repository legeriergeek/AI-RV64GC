CC = gcc
TARGET = riscv-emu

# Désactive les hardening flags injectés par le système (cf-protection, stack-protector, etc.)
# Ces flags sont catastrophiques pour un interpréteur à dispatch indirect (goto *table[opcode])
CFLAGS = -O3 -march=native -flto=auto \
         -funroll-loops \
         -fomit-frame-pointer \
         -fno-stack-protector \
         -fno-stack-clash-protection \
         -fcf-protection=none \
         -fno-plt \
         -fno-semantic-interposition \
         -falign-functions=64 \
         -falign-loops=32 \
         -fno-math-errno \
         -Wall -Wextra -Wno-unused-parameter \
         -I include

# PGO : décommente la phase appropriée
# Phase 1 - génération du profil :
#   make PGO=generate
# Phase 2 - utilisation du profil (après avoir lancé l'émulateur quelques minutes) :
#   make PGO=use
ifdef PGO
  ifeq ($(PGO),generate)
    CFLAGS  += -fprofile-generate
    LDFLAGS_EXTRA = -fprofile-generate
  else ifeq ($(PGO),use)
    CFLAGS  += -fprofile-use -fprofile-correction
    LDFLAGS_EXTRA = -fprofile-use
  endif
endif

LDFLAGS = -O3 -flto=auto -lm -lpthread \
          -fno-semantic-interposition \
          $(LDFLAGS_EXTRA)

SRC = src/main.c src/cpu.c src/execute.c src/compressed.c \
      src/bus.c src/dram.c src/uart.c src/clint.c \
      src/plic.c src/virtio.c src/virtio_net.c src/mmu.c src/dtb.c

OBJ = $(SRC:.c=.o)

.PHONY: all clean pgo-clean help

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Affiche les flags réels utilisés par GCC (utile pour debugger les flags système)
check-flags:
	$(CC) $(CFLAGS) -Q --help=optimizers 2>/dev/null | grep -E 'cf-protection|stack-protector|stack-clash|omit-frame'

pgo-clean:
	rm -f src/*.gcda src/*.gcno

clean: pgo-clean
	rm -f $(OBJ) $(TARGET)

help:
	@echo "Targets:"
	@echo "  make              - build normal"
	@echo "  make PGO=generate - build avec instrumentation PGO"
	@echo "  make PGO=use      - build optimisé avec le profil collecté"
	@echo "  make check-flags  - vérifie les flags hardening actifs"
	@echo "  make pgo-clean    - supprime les données de profil"
