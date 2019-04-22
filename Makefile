CC = gcc
CCARGS = -Wall -O2 -g `pkg-config fuse --cflags --libs`

csc452: csc452fuse.c
	$(CC) $(CCARGS) -o $@ $^

#make a new .disk and wipe the old one
newdisk:
	dd bs=1K count=5K if=/dev/zero of=.disk

.PHONY: clean
clean:
	rm -r csc452
