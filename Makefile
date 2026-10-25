TARGET = Tetris
OBJS   = main.o

CFLAGS   = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBS = -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = Tetris // Hack
PSP_EBOOT_ICON  = ICON0.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
