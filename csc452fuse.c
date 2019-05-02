/*
	FUSE: Filesystem in Userspace


	gcc -Wall `pkg-config fuse --cflags --libs` csc452fuse.c -o csc452


*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>

static const char *diskpath = ".disk";

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

typedef struct
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct csc452_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct csc452_file_directory) - sizeof(int)];
} csc452_directory_entry;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

typedef struct
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct csc452_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE
		- MAX_DIRS_IN_ROOT * sizeof(struct csc452_directory)
		- sizeof(int)];
} csc452_root_directory;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)

typedef struct
{
	//All of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
} csc452_disk_block;

//read block allocation data from the end of the disk and load it into a linked
//list starting at *head
//return length of allocation bitmap
int *readAllocationData(FILE *disk, unsigned long *allocLength)
{
	if (!disk)
		return 0;

	//save position in file
	long cur = ftell(disk);

	//get disk size
	unsigned long diskSize;
	fseek(disk, 0, SEEK_END);
	diskSize = ftell(disk);
	//get length of allocation bitmap (round up to nearest byte)
	*allocLength = ((diskSize / BLOCK_SIZE) + 7) / 8;
	//get offset from end where allocation data starts (round up to nearest block)
	unsigned int allocOffset = (*allocLength + BLOCK_SIZE - 1)/BLOCK_SIZE;

	fseek(disk, allocOffset, SEEK_END);
	int *arr = calloc(1, *allocLength);
	if (!arr)
		exit(1);
	fread(arr, *allocLength, 1, disk);

	//go back to where we started
	fseek(disk, cur, SEEK_SET);

	return arr;
}

void writeAllocationData(FILE *disk, int *arr, unsigned long allocLength)
{
	if (!disk)
		return;
	//save position in file
	long cur = ftell(disk);

	//get offset from end where allocation data will go (round up to nearest block)
	unsigned int allocOffset = (allocLength + BLOCK_SIZE - 1)/BLOCK_SIZE;

	fseek(disk, allocOffset, SEEK_END);
	fwrite(arr, allocLength, 1, disk);

	//go back to where we started
	fseek(disk, cur, SEEK_SET);
}

void setBit(int *arr, int k) {
	arr[k/32] |= 1 << (k % 32);
}

void clearBit(int *arr, int k) {
	arr[k/32] &= ~(1 << (k % 32));
}

int testBit(int *arr, int k) {
	return ((arr[k/32] & (1 << (k % 32))) != 0);
}

FILE *load_rd(csc452_root_directory *root) {
	FILE *fp = fopen(diskpath, "r+b");
	if (!fp)
		return (FILE *)0;
	return fp;
}

FILE *load_dir(csc452_directory_entry *dir, long blk) {
	FILE *fp = fopen(diskpath, "r+b");
	if (!fp)
		return (FILE *)0;
	fseek(fp, blk * BLOCK_SIZE, SEEK_SET);
	return fp;
}

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not.
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int csc452_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid();
	stbuf->st_atime = time(NULL);
	stbuf->st_mtime = time(NULL);

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {
		csc452_root_directory rd;
		FILE *disk = fopen(diskpath, "r+b");
		if (!disk)
			return -EFAULT;
		fread(&rd, sizeof(csc452_root_directory), 1, disk);

		//break path into dir/fn/ext
		char directory[MAX_FILENAME+1] = "\0";
		char filename[MAX_FILENAME+1] = "\0";
		char extension[MAX_EXTENSION+1] = "\0";
		sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

		long dirstart = 0;
		for (int i = 0; i < rd.nDirectories; i++) {
			if (!strcmp((char *)(rd.directories[i].dname), directory)) {
				dirstart = rd.directories[i].nStartBlock;
				break;
			}
		}
		//directory doesn't exist
		if (!dirstart) {
			fclose(disk);
			return -ENOENT;
		}
		//No filename - path exists and is a directory
		if (!strcmp(filename, "")) {
			stbuf->st_mode = S_IFDIR | 0755;
			stbuf->st_nlink = 2;
			fclose(disk);
			return res;
		}
		//Filename - path exists and we're looking for a file
		else {
			//read in dir
			fseek(disk, dirstart * BLOCK_SIZE, SEEK_SET);
			csc452_directory_entry dir;
			fread(&dir, sizeof(csc452_directory_entry), 1, disk);
			for (int i = 0; i < dir.nFiles; i++) {
				if (!strcmp((char *)(dir.files[i].fname), filename)
						&& !strcmp((char *)(dir.files[i].fext), extension)) {
					stbuf->st_mode = S_IFREG | 0666;
					stbuf->st_nlink = 2;
					stbuf->st_size = dir.files[i].fsize;
					fclose(disk);
					return res;
				}
			}

		}

		//Else return that path doesn't exist
		fclose(disk);
		res = -ENOENT;
	}

	return res;
}

/*
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int csc452_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	//(void) offset;
	(void) fi;

	csc452_root_directory rd;
	FILE *disk = fopen(diskpath, "rb");
	if (!disk)
		return -EFAULT;
	fread(&rd, sizeof(csc452_root_directory), 1, disk);

	//A directory holds two entries, one that represents itself (.) 
	//and one that represents the directory above us (..)
	if (strcmp(path, "/") != 0) {
		for (int i = 0; i < rd.nDirectories; i++) {
			if (!strcmp((char *)(rd.directories[i].dname), strtok((char *)path, "/"))) {
				fseek(disk, rd.directories[i].nStartBlock, SEEK_SET);
				csc452_directory_entry dir;
				fread(&dir, sizeof(csc452_directory_entry), 1, disk);
				filler(buf, ".", NULL,0);
				filler(buf, "..", NULL, 0);
				for (int j = 0; j < dir.nFiles; j++)
					filler(buf, dir.files[j].fname, NULL, 0);
				fclose(disk);
				return 0;
			}
		}
	}
	else {
		filler(buf, ".", NULL,0);
		filler(buf, "..", NULL, 0);
		//read each subdirectory into buffer
		//disregard last two params because we don't care about file attributes
		//and we're not doing weird things with string offsets(?)
		for (int i = 0; i < rd.nDirectories; i++) {
			filler(buf, rd.directories[i].dname, NULL, 0);
		}
		fclose(disk);
		return 0;
	}

	fclose(disk);
	return -ENOENT;
}

/*
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int csc452_mkdir(const char *path, mode_t mode)
{
	(void) mode;

	if (strlen(path) > MAX_FILENAME + 1)
		return -ENAMETOOLONG;

	csc452_root_directory rd;
	FILE *disk = fopen(diskpath, "r+b");
	if (!disk)
		return -EFAULT;
	fread(&rd, sizeof(csc452_root_directory), 1, disk);
	//check whether we can make more directories
	if (rd.nDirectories >= MAX_DIRS_IN_ROOT) {
		fclose(disk);
		return -ENOSPC;
	}
	//check whether directory already exists
	for (int i = 0; i < rd.nDirectories; i++) {
		if (strncmp(rd.directories[i].dname, strtok((char *)path, "/"), MAX_FILENAME) == 0) {
			fclose(disk);
			return -EEXIST;
		}
	}

	//ensure directory is in root (enforce two-level tree)
	//TODO

	//read free space data and find an open block to put the directory in
	//FIXME: writing bits to random arbitrary positions
	unsigned long allocLength = 0;
	int *allocData = NULL;
	allocData = readAllocationData(disk, &allocLength);
	long startBlk = 1;
	while (testBit(allocData, startBlk))
		startBlk++;

	//create the directory
	csc452_directory_entry dir_e = {
		.nFiles = 0
	};

	setBit(allocData, startBlk);
	writeAllocationData(disk, allocData, allocLength);
	free(allocData);
	//update directory list in root
	strncpy(rd.directories[rd.nDirectories].dname, strtok((char *)path, "/"), MAX_FILENAME + 1);
	rd.directories[rd.nDirectories].nStartBlock = startBlk;
	rd.nDirectories++;

	//write back to disk
	rewind(disk);
	fwrite(&rd, sizeof(csc452_root_directory), 1, disk);
	fseek(disk, startBlk * BLOCK_SIZE, SEEK_SET);
	fwrite(&dir_e, sizeof(csc452_directory_entry), 1, disk);
	fclose(disk);

	return 0;
}

/*
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 * Note that the mknod shell command is not the one to test this.
 * mknod at the shell is used to create "special" files and we are
 * only supporting regular files.
 */
static int csc452_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) path;
	(void) mode;
    (void) dev;
	printf("*** called mknod\n");

	//break path into dir/fn/ext
	char directory[MAX_FILENAME+1] = "\0";
	char filename[MAX_FILENAME+1] = "\0";
	char extension[MAX_EXTENSION+1] = "\0";
	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	printf("*** dir: %s\n*** fn: %s\n*** ext: %s\n", directory, filename, extension);
	if (strlen(filename) > MAX_FILENAME + 1 || strlen(extension) > MAX_EXTENSION + 1)
		return -ENAMETOOLONG;

	if (strcmp(directory, "") == 0)
		return -EPERM;

	csc452_root_directory rd;
	FILE *disk = fopen(diskpath, "r+b");
	if (!disk)
		return -EFAULT;
	fread(&rd, sizeof(csc452_root_directory), 1, disk);

	//check that directory exists and load it
	long dirstart = 0;
	int i = 0;
	for (; i < rd.nDirectories; i++) {
		if (!strcmp((char *)(rd.directories[i].dname), directory)) {
			dirstart = rd.directories[i].nStartBlock;
			break;
		}
	}
	//directory doesn't exist
	if (!dirstart) {
		fclose(disk);
		return -ENOENT;
	}

	//read in dir
	fseek(disk, dirstart * BLOCK_SIZE, SEEK_SET);
	csc452_directory_entry dir;
	fread(&dir, sizeof(csc452_directory_entry), 1, disk);
	//update dir
	//check that filename doesn't already exist in directory
	for (int j = 0; j < dir.nFiles; j++) {
		if (!strcmp((char *)(dir.files[i].fname), filename)
				&& !strcmp((char *)(dir.files[i].fext), extension)) {
			fclose(disk);
			return -EEXIST;
		}
	}
	strcpy(dir.files[dir.nFiles].fname, filename);
	strcpy(dir.files[dir.nFiles].fext, extension);
	dir.files[dir.nFiles].fsize = 0;
	//read free space data and find an open block to put the file in
	unsigned long allocLength = 0;
	int *allocData = NULL;
	allocData = readAllocationData(disk, &allocLength);
	long startBlk = 1;
	while (testBit(allocData, startBlk))
		startBlk++;
	dir.files[dir.nFiles].nStartBlock = startBlk;
	dir.nFiles++;

	//write dir back
	fseek(disk, dirstart * BLOCK_SIZE, SEEK_SET);
	fwrite(&dir, sizeof(csc452_directory_entry), 1, disk);

	fclose(disk);
	return 0;
}

/*
 * Read size bytes from file into buf starting from offset
 */
static int csc452_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//return success, or error

	return size;
}

/*
 * Write size bytes from buf into file starting from offset
 */
static int csc452_write(const char *path, const char *buf, size_t size,
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//write data
	//return success, or error

	return size;
}

/*
 * Removes a directory (must be empty)
 */
static int csc452_rmdir(const char *path)
{
	(void) path;

	int ret = 0;
	int found = 0;

	csc452_root_directory rd;
	FILE *disk = fopen(diskpath, "r+b");
	if (!disk)
		return -EFAULT;
	fread(&rd, sizeof(csc452_root_directory), 1, disk);
	//find the directory
	for (int i = 0; i < rd.nDirectories; i++) {
		if (strncmp(rd.directories[i].dname, strtok((char *)path, "/"), MAX_FILENAME) == 0) {
			//remove from dir list, ensuring array is packed
			if (i < rd.nDirectories - 1)
				rd.directories[i] = rd.directories[rd.nDirectories - 1];
			rd.nDirectories--;
			found = 1;
			break;
		}
	}

	if (found) {
		//write back to disk
		rewind(disk);
		fwrite(&rd, sizeof(csc452_root_directory), 1, disk);
		//TODO: write to free space tracking
	}
	//not found
	else
		ret = -ENOENT;

	fclose(disk);
	return ret;
}

/*
 * Removes a file.
 */
static int csc452_unlink(const char *path)
{
        (void) path;
        return 0;
}


/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int csc452_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}

/*
 * Called when we open a file
 *
 */
static int csc452_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int csc452_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations csc452_oper = {
    .getattr	= csc452_getattr,
    .readdir	= csc452_readdir,
    .mkdir		= csc452_mkdir,
    .read		= csc452_read,
    .write		= csc452_write,
    .mknod		= csc452_mknod,
    .truncate	= csc452_truncate,
    .flush		= csc452_flush,
    .open		= csc452_open,
    .unlink		= csc452_unlink,
    .rmdir		= csc452_rmdir
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &csc452_oper, NULL);
}
