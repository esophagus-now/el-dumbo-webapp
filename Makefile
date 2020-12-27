main: main.c
	gcc -g -o main -Wall -fno-diagnostics-show-caret main.c implement.c -I/home/mahkoe/projects/mmlib -levent -lcrypto

clean:
	rm -rf main
