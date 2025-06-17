EXPORT_DIR = ../../../engine/export

all: sndh_regdump.bin

sndh_regdump.bin: sndh_regdump.s
	vasmm68k_mot -m68000 -pic -Fbin -o $@ $<
	echo "#include \"sndh.h\"" > $(EXPORT_DIR)/player_sndh_regdump.cpp
	xxd -i -n "player_sndh_regdump" sndh_regdump.bin | sed -r "s/unsigned/const unsigned/" >> $(EXPORT_DIR)/player_sndh_regdump.cpp
