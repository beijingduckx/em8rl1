
fx2firm.inc: gpif.c em8rl1.c
	sdcc -mmcs51 -c ./gpif.c
	sdcc -mmcs51 -c ./em8rl1.c
	sdcc em8rl1.rel gpif.rel
	sed -e 's/.*/"&",/' < em8rl1.ihx > fx2firm.inc

clean:
	-rm *.ihx
	-rm *.inc
	-rm *.rel
	-rm *.sym
	-rm *.mem
	-rm *.map
	-rm *.asm
	-rm *.lst
	-rm *.rst
	-rm *.lk


