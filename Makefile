build: kai.c
	$(CC) kai.c -o kai -Wall -Wextra -pedantic -std=c17
tool:
	$(CC) charcode.c -o charcode -Wall -Wextra -pedantic -std=c17
clean:
	rm -f kai charcode
