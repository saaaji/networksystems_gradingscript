all: dfc dfs

dfc: dfc.c dfc_maps.c
	gcc -Wall -Wextra -o dfc dfc.c dfc_maps.c -lssl -lcrypto

dfs: dfs.c
	gcc -Wall -Wextra -o dfs dfs.c

clean:
	rm -f dfc dfs *.o
