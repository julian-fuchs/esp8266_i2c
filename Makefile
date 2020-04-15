
BUILD=./build
SRC=./src
INCLUDE=./include

CC = xtensa-lx106-elf-gcc
CFLAGS = -I$(INCLUDE) -mlongcalls -Wall
LDLIBS = -nostdlib -Wl,--start-group -lmain -lnet80211 -lwpa -llwip -lpp -lphy -lc -Wl,--end-group -lgcc
LDFLAGS = -Teagle.app.v6.ld

SRCS=$(wildcard $(SRC)/*.c)
OBJS=$(SRCS:$(SRC)/%.c=$(BUILD)/%.o)
BINARY=$(BUILD)/esp8266_i2c

.PHONY: flash
flash: $(BINARY)-0x00000.bin
	esptool.py  -p /dev/ttyUSB0 -b 460800 \
	write_flash --flash_freq 80m --flash_mode qio \
	0x00000 $(BINARY)-0x00000.bin \
	0x10000 $(BINARY)-0x10000.bin \
	0x3fc000 esp_init_data.bin

.PHONY: link
link: $(BINARY)

$(BINARY)-0x00000.bin: clean $(BINARY)
	esptool.py elf2image $(BINARY)

$(BINARY): compile
	$(CC) $(LDFLAGS) $(BUILD)/main.o $(BUILD)/blink.o $(LOADLIBES) $(LDLIBS) -o $(BINARY)

.PHONY: compile
compile:
	for src in $(SRCS); do \
		obj=`echo $$src | sed -e "s/\.c/.o/g" | sed -e "s@$(SRC)@$(BUILD)@g"`; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -c $$src   -o $$obj; \
	done

.PHONY: clean
clean:
	rm -f $(BUILD)/*

