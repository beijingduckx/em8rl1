
sdcc -mmcs51 -c ./gpif.c
sdcc -mmcs51 -c ./em8rl1.c
sdcc em8rl1.rel gpif.rel

./gen_inc.ps1 > fx2firm.inc
