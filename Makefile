# PARLZ Editor (pee) -- 单文件终端编辑器
#   Windows: 需要 clang/gcc + user32(剪贴板);  MinGW 同样可用
#   类 Unix: cc pee.c -o pee
CFLAGS ?= -O2 -Wall -Wextra

ifeq ($(OS),Windows_NT)
TARGET = pee.exe
LIBS   = -luser32 -lkernel32 -lgdi32
else
TARGET = pee
LIBS   =
endif

all: $(TARGET)

$(TARGET): pee.c
	$(CC) $(CFLAGS) -o $@ pee.c $(LIBS)

clean:
	-$(RM) $(TARGET)

.PHONY: all clean
