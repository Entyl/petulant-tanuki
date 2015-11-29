/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.

*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define	MAX_FILES_IN_DIR (BLOCK_SIZE - (MAX_FILENAME + 1) - sizeof(int)) / \
	((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//Amount of total DISK_BLOCKS
#define MAX_BLOCKS 10220

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK BLOCK_SIZE

//How many pointers in an inode?
#define NUM_POINTERS_IN_INODE ((BLOCK_SIZE - sizeof(unsigned int) - sizeof(unsigned long)) / sizeof(unsigned long))

struct cs1550_directory_entry
{
	char dname[MAX_FILENAME	+ 1];	//the directory name (plus space for a nul)
	int nFiles;			//How many files are in this directory. 
					//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;			//file size
		long nStartBlock;		//where the first block is on disk
	} files[MAX_FILES_IN_DIR];		//There is an array of these
};

typedef struct cs1550_directory_entry cs1550_directory_entry;

struct cs1550_disk_block
{
	//And all of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;

struct bitmap	//bitmap to hold, total size in bytes = 10224 (4 + 10220), out of 10240(512 * 20) available because of block size being 512  
{
	unsigned char tracker[ 20 * MAX_DATA_IN_BLOCK - 20 ];	//512 * 20 - 20= 10220 total blocks able to be track
};

typedef struct bitmap bitmap;	//bit map will be written to the end of the .disk file

struct meta_entry	//holds a cs1550_directory_entry with some metadata to help the file functions
{
	int file_index;	//this is the file index that is set and -1 if not a file or file not found
	int index_of_directory;	//tells what index the directory is at and -1 if directory is not found
	int slash_count;	//if slash count is greater than 1 than not under root
};

typedef struct meta_entry meta_entry;

//fuction prototypes
int locate_directory(char *directory);
int locate_file(char *directory, char *filename, char *extension);
long get_first_free_block(void);
int is_next_block_free(long start_address);
long get_start_address(cs1550_directory_entry directory, int file_index);
long find_next_free_block(long start_address);
cs1550_directory_entry move_file(cs1550_directory_entry directory, int file_index);
void write_directory_entry(cs1550_directory_entry current_directory, int index);

static cs1550_directory_entry get_directory_entry(int index)	//returns the directory entry at the index
{
	FILE *directory_list;	//file
	cs1550_directory_entry current_directory;	//holds the directory
	
	directory_list = fopen(".directories", "rb");

	fseek(directory_list, index * sizeof(current_directory), SEEK_SET );	//seeks to entry
	fread(&current_directory, sizeof(current_directory), 1, directory_list);

	fclose(directory_list);
	
	return current_directory;
}

int locate_directory(char *directory)	//locates and returns index of struct in .directories, -1 means not located
{
	FILE *directory_list;	//file
	cs1550_directory_entry current_directory;	//directory to be read
	int index_of_directory = -1;
	int count = 0;	//count of indexes

	directory_list = fopen(".directories", "rb");	//open .directories
	
	if(directory_list != NULL)
	{
		while( fread(&current_directory, sizeof(current_directory), 1, directory_list) == 1 )	//reads a directory_entry struct
		{
			if( strcmp(current_directory.dname+1, directory) == 0 )	//checks if current_directory is the directory we are looking for
			{
				index_of_directory = count;
				fclose(directory_list);	//close
				break;
			}
			count++;
		}
	}	
	return index_of_directory;
}

int locate_file(char *directory, char *filename, char *extension)	//locates and returns index of file in directory, -1 means not located
{
	int index_of_file = -1;
	int count;	//count of file indexes
	int index_of_directory = locate_directory( directory );	//locates the directory that holds supposive file

	if(index_of_directory > -1)	//means the directory is actually there
	{
		cs1550_directory_entry current_directory = get_directory_entry( index_of_directory );	//get directory
	
		for(count = 0; count < current_directory.nFiles; count++)
		{
			//checks if filename and extension match
			if( strcmp(filename, current_directory.files[ count ].fname) == 0 && strcmp(extension, current_directory.files[ count ].fext) == 0 )
			{
				index_of_file = count;
				break;
			}
		}
	}
	return index_of_file;
}

static meta_entry find_correct_directory(const char *path)	//finds and returns the correct directory that the calling funciton wanted by reading through .directories
{
	char directory[MAX_FILENAME + 1];	//name of directory we are looking for
	char filename[MAX_FILENAME + 1];	//name of file we are looking for
	char extension[MAX_EXTENSION + 1];	//file extension we are looking for
	int counter = 0;

	meta_entry search_return;	//the meta_entry struct that holds the directory to be returned
	search_return.file_index = -1;	//set so that parameter path is also not a file index
	search_return.index_of_directory = -1;	//starts at -1, and if its an actually directory it will be set to a real index
	search_return.slash_count = 0;

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);	//tokenizes and stores the strings for the directory we are looking for

	while( counter < strlen(path) ) //to check the directory is not under root
	{
               if(path[counter] == '/')
               {
                       search_return.slash_count++;
               }
               counter++;
       	}

	if( search_return.slash_count > 1)	//path is not under root
	{
		search_return.file_index = locate_file(directory, filename, extension);	//finds file
	}

	search_return.index_of_directory = locate_directory(directory);	//finds directory

	return search_return;
}

void write_directory_entry(cs1550_directory_entry current_directory, int index)
{
	FILE *directory_list;	//file
	directory_list = fopen(".directories", "r+b");

	fseek(directory_list, index * sizeof(current_directory), SEEK_SET );	//seeks to entry
	fwrite(&current_directory, sizeof(current_directory), 1, directory_list);	//rewrite the struct				

	fclose(directory_list);
	
	return;
}

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not. 
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	int res = -ENOENT;	//sets res to error first, if the directory or file is found then res is set to 0 because there is no error

	memset(stbuf, 0, sizeof(struct stat));
	//is path the root dir?
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		res = 0;
	} 

	else	//is directly under root directory
	{
		meta_entry attribute = find_correct_directory(path);

		if(attribute.index_of_directory > -1 && attribute.slash_count == 1)	//means that paramter path is a directory
		{
			//Might want to return a structure with these fields
			stbuf->st_mode = S_IFDIR | 0755;
			stbuf->st_nlink = 2;
			res = 0;	
		}

		else if(attribute.index_of_directory > -1 && attribute.file_index > -1)//means that parameter path is a file
		{
			//regular file, probably want to be read and write
			cs1550_directory_entry directory_entry = get_directory_entry( attribute.index_of_directory );
			stbuf->st_mode = S_IFREG | 0666; 
			stbuf->st_nlink = 1; //file link
			stbuf->st_size = directory_entry.files[ attribute.file_index ].fsize; //file size - make sure you replace with real size!
			res = 0; // no error
		}
	}

	return res;
}

/* 
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;
	int res = -ENOENT;
	
	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	if ( strcmp(path, "/") == 0 )	//means that the path is root, so filler() all subdirectories
	{
		FILE *directory_list;
		cs1550_directory_entry current_directory;	//current directory to be printed out
		
		directory_list = fopen(".directories", "rb");	//open .directories
		
		if(directory_list != NULL)
		{
			while( fread(&current_directory, sizeof(current_directory), 1, directory_list) == 1 )
			{
				filler(buf, current_directory.dname + 1, NULL, 0);
			}
			fclose(directory_list);
		}
		res = 0;
	}

	else
	{
		//add the user stuff (subdirs or files)
		//the +1 skips the leading '/' on the filenames
		meta_entry attribute = find_correct_directory(path);
			
		if(attribute.index_of_directory > -1)	//means that parameter path is a directory(aka subdirectory of root)
		{
			cs1550_directory_entry directory_entry = get_directory_entry( attribute.index_of_directory );
			
			int count;
			for(count = 0; count < directory_entry.nFiles; count++)	//go through filler() the files
			{
				char fullname[12] = "";
				sprintf(fullname, "%s.%s", directory_entry.files[count].fname, directory_entry.files[count].fext);
				filler(buf, fullname, NULL, 0);
			}
			res = 0;
		}
	}
	return res;
}

/* 
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{
	int res = 0;
	(void) mode;

	meta_entry attribute = find_correct_directory(path);

	if( attribute.slash_count > 1 )	//means not under root, do not give permission
	{
		res = -EPERM;
	}

	else if( strlen(path) > 9 )	//name length is too long, 9 because of beginning "/"
	{
		res = -ENAMETOOLONG;
	}

	else if( attribute.index_of_directory > -1 && attribute.file_index == -1)	//is a directory
	{
		res = -EEXIST;
	}

	else	//append
	{
		FILE *directory_list;
		cs1550_directory_entry current_directory;	//current directory to be appended
		directory_list = fopen(".directories", "ab");	//open(or create if it does not exist) to append .directories
		
		sprintf(current_directory.dname, "%s", path);
		current_directory.nFiles = 0;
		fwrite(&current_directory, sizeof(current_directory), 1, directory_list);	//append directory				
		fclose(directory_list);
	}
	return res;
}

/* 
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
	return 0;
}

long get_first_free_block(void)
{
	FILE *disk;
	bitmap bitmap;	//bitmap to read, edit and write
	long count = 0;	//will count until it finds a free block
	
	disk = fopen(".disk", "r+b");
	
	fseek(disk, -1 * sizeof(bitmap), SEEK_END);	//seeks to end and -1 size of bitmap to read in
	fread(&bitmap, sizeof(bitmap), 1, disk);	//read in bitmap
	
	for(; count < MAX_BLOCKS; count++)
	{
		if( bitmap.tracker[ count ] == '\0' )	//free
		{
			bitmap.tracker[ count ] = '1';	//now in use
			break;
		}		
	}

	int check;
	fseek(disk, -1 * sizeof(bitmap), SEEK_END);	//seeks to end and -1 size of bitmap to read in
	check = fwrite(&bitmap, sizeof(bitmap), 1, disk);	//read into bitmap
	fclose(disk);

	return count * 512;	//returns the addres of the block
}
/* 
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;
	char directory[MAX_FILENAME + 1];	//name of directory we are looking for
	char filename[MAX_FILENAME + 1];	//name of file we are looking for
	char extension[MAX_EXTENSION + 1];	//file extension we are looking for
	
	int res = 0;
	meta_entry attribute = find_correct_directory(path);  

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);	//tokenizes and stores the strings for the directory we are looking for	

	if( attribute.slash_count == 1 )	//under root, do not give permission
	{
		res = -EPERM;
	}

	else if( strlen(filename) > 8 || strlen(extension) > 3 )	//name length is too long
	{
		res = -ENAMETOOLONG;
	}

	else if( attribute.file_index > -1)	//is already a file
	{
		res = -EEXIST;
	}

	else	//create file
	{
		cs1550_directory_entry directory_entry = get_directory_entry( attribute.index_of_directory );
				
		strcpy(directory_entry.files[ directory_entry.nFiles ].fname, filename); //put filename in array index
		strcpy(directory_entry.files[ directory_entry.nFiles ].fext, extension);	//put extension in array index
//		directory_entry.files[ directory_entry.nFiles ].fsize = 512;	//size of block

		directory_entry.files[ directory_entry.nFiles ].fsize = 0;	//size of block
		directory_entry.files[ directory_entry.nFiles ].nStartBlock = get_first_free_block();	//gets the first free block
		directory_entry.nFiles = directory_entry.nFiles++;	//increment the amount of files  

		write_directory_entry( directory_entry, attribute.index_of_directory );	//write the directory entry			
		
	}

	return res;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
	int res = 0;
	meta_entry attribute = find_correct_directory(path);  

	if( attribute.slash_count == 1 )	//the path is a directory, overwrite res
	{
		res = -EISDIR;
	}

 	else if( attribute.index_of_directory > 1 && attribute.file_index == -1 )	//file not found(or wrong path but goes under same error)
	{
		res = -ENOENT;
	}   	
	
	else	//remove file
	{
		int index = attribute.file_index;
		cs1550_directory_entry directory_entry = get_directory_entry( attribute.index_of_directory );

		//collasce the array
		for( ; index < directory_entry.nFiles-1; index++)
		{
			strcpy( directory_entry.files[ index ].fname, directory_entry.files[ index + 1 ].fname );
			strcpy( directory_entry.files[ index ].fext, directory_entry.files[ index + 1 ].fext );
			directory_entry.files[ index ].fsize = directory_entry.files[ index + 1 ].fsize;
			directory_entry.files[ index ].nStartBlock = directory_entry.files[ index + 1 ].nStartBlock;
		}
		
		directory_entry.nFiles = directory_entry.nFiles--;	//remove the file from count

		write_directory_entry( directory_entry, attribute.index_of_directory );	//write the directory entry			

	}
	return res;
}

/* 
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) fi;
	meta_entry attribute = find_correct_directory(path);

	//check to make sure path exists
	if(attribute.index_of_directory > -1 && attribute.slash_count == 1)	//means that paramter path is a directory
	{
		size = -EISDIR;
	}
	
	else
	{
		int index = attribute.file_index;
		cs1550_directory_entry directory_entry = get_directory_entry( attribute.index_of_directory );
		
		if( size > 0 && offset <= directory_entry.files[index].fsize )	//check that size is > 0 and offset is <= to the file size
		{
			//read in data
			FILE *disk;
			disk = fopen(".disk", "rb");

			fseek(disk, directory_entry.files[ index ].nStartBlock + offset, SEEK_SET);	//seek from start to where the first block is plus offset
			fread(buf, sizeof(char), sizeof(buf), disk);	//read in the amount of bytes needed			
 			size = strlen(buf);	//set size and return, or error

			fclose(disk);	
		}
		
		else
		{
			size = -1;	//error
		}
	}
	return size;
}

int is_next_block_free(long start_address)	//returns 1 if the next block is free, returns 0 if it is not
{
	FILE *disk;
	bitmap bitmap;	//bitmap to read, edit and write
	int next_bitmap_index = (start_address / 512) + 1;	//gets the index of the next block
	int freedom = 0;	//by default set to 0, so the block is not free

	disk = fopen(".disk", "r+b");
	
	fseek(disk, -1 * sizeof(bitmap), SEEK_END);	//seeks to end and -1 size of bitmap to read in
	fread(&bitmap, sizeof(bitmap), 1, disk);	//read in bitmap
	
	if( bitmap.tracker[ next_bitmap_index ] == '\0' )	//free
	{
		freedom = 1;
	}
	fclose(disk);

	return freedom;	
}

long find_next_free_block(long start_address)	//finds address of next free block because next block returned false
{
	FILE *disk;
	bitmap bitmap;	//bitmap to read, edit and write
	long index = (start_address / 512);	//gets the index of the start block

	disk = fopen(".disk", "r+b");
	
	fseek(disk, -1 * sizeof(bitmap), SEEK_END);	//seeks to end and -1 size of bitmap to read in
	fread(&bitmap, sizeof(bitmap), 1, disk);	//read in bitmap
	
	for(; index < MAX_BLOCKS; index++)
	{
		if( bitmap.tracker[ index ] == '\0' )	//free
		{
			start_address = index * 512;	//found free block so set address and break 
			break;
		}		
	}
	fclose(disk);

	return start_address;	//returns the addres of the block

}

long get_start_address(cs1550_directory_entry directory, int file_index)	//gives the new start address(if needed) of file
{
	long count = 0;
	long start_address = directory.files[ file_index ].nStartBlock;	//holds the start address of the file
	long error_checker_address = start_address;	

	while( count <= directory.files[ file_index ].fsize )
	{
		int check = is_next_block_free(start_address);
		if(check != 0)	//next block is free
		{
			count+= 512;	//add a block size for every iteration
		}
		
		else
		{
			start_address = find_next_free_block(start_address);	//finds next free block and sets block_start to that address
			if(error_checker_address == start_address)	//signalfies out of mememory
			{
				perror("OUT OF MEMORY!!\n");	//out of memeory
				break;
			}
			count = 0;	//reset count
		}
	}

	return start_address;	//returns the new start address
}

cs1550_directory_entry move_file(cs1550_directory_entry directory, int file_index)	//moves the given file
{
	FILE *disk;
	cs1550_disk_block block;	//block that holds information from parts of disk
	bitmap bitmap;	//bitmap to read, edit and write
	long new_address = get_start_address( directory, file_index );
	long count = 0;
	disk = fopen(".disk", "r+b");

	fseek(disk, -1 * sizeof(bitmap), SEEK_END);	//seeks to end and -1 size of bitmap to read in
	fread(&bitmap, sizeof(bitmap), 1, disk);	//read in bitmap		

	while( count <= directory.files[ file_index ].fsize )	//note that this fsize is the old offset
	{
		fseek(disk, directory.files[ file_index ].nStartBlock + count, SEEK_SET);	//seek from start to where current block is
		fread(&block.data, sizeof(char), sizeof(block.data), disk);	//read in a block
	
		bitmap.tracker[ (directory.files[ file_index ].nStartBlock + count) / 512 ] = '0';	//sets the block to free
	
		fseek(disk, new_address + count, SEEK_SET);	//seek from start to new block on disk
		fwrite(&block.data, sizeof(char), sizeof(block.data), disk);	//write data to new block			

		bitmap.tracker[ (new_address + count) / 512 ] = '1';	//sets the new block to used

		count += 512;	//increment count
	}

	fseek(disk, -1 * sizeof(bitmap), SEEK_END);	//seeks to end and -1 size of bitmap to read in
	fwrite(&bitmap, sizeof(bitmap), 1, disk);	//writes into bitmap
	fclose(disk);
	
	directory.files[ file_index ].nStartBlock = new_address;	//puts new address in entry
	return directory;
}

/* 
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size, 
			  off_t offset, struct fuse_file_info *fi)
{
	(void) fi;
	meta_entry attribute = find_correct_directory(path);
	int index = attribute.file_index;
	cs1550_directory_entry directory_entry = get_directory_entry( attribute.index_of_directory );	

	if( index == -1)	//check to make sure path exists
	{
		size = -1;	//error
	}
	
	else if(  && offset <= directory_entry.files[index].fsize )	//check that offset is <= to the file size and file size is greater than 0
	{
		//write data
		FILE *disk;
		cs1550_disk_block block;	//block to be written
		disk = fopen(".disk", "r+b");
			
		fseek(disk, directory_entry.files[ index ].nStartBlock + offset, SEEK_SET);	//seek from start to where the first block is plus offset
		strcpy(block.data, buf);
		fwrite(&block.data, sizeof(char), sizeof(buf), disk);	//write data			

		size = strlen(buf);	//return the length of data
		fclose(disk);	

		directory_entry.files[index].fsize = size;
		write_directory_entry( directory_entry, attribute.index_of_directory);
	}

	else if( offset <= directory_entry.files[index].fsize )	//append the file
	{
		//hopefully how many writes is only 1 becuase it will be poor performance otherwise 
		int how_many_writes = (offset - directory_entry.files[index].fsize) / 512 + 1;	//how many times to copy
		int count = 0;
		FILE *disk;
		cs1550_disk_block block;

		FILE *directory_list;	//to edit and sent directory
		cs1550_directory_entry directory = get_directory_entry( attribute.index_of_directory );	//get directory
		while(count < how_many_writes)
		{
			move_file( directory, index);
			disk = fopen(".disk", "r+b");
			strncpy(block.data, buf + count * 512, 512);	//copies up to \0 or 512 bytes
			
			fwrite(&block.data, sizeof(char), sizeof(block.data), disk);	//write the new data
			count++;
			directory.files[index].fsize = directory_entry.files[index].fsize + 512;	//update size
			fclose(disk);
		}
		directory_list = fopen(".directories", "r+b");	
		fseek(directory_list, index * sizeof(directory_entry), SEEK_SET );	//seek to correct direcotry entry
		fwrite(&directory_entry, sizeof(directory_entry), 1, directory_list);	//rewrite the struct				
		fclose(directory_list);
	}

	else
	{
		size = -EFBIG;		
	}
	//set size (should be same as input) and return, or error

	return size;
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
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/* 
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
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
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
	.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
	.mknod	= cs1550_mknod,
	.unlink = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush = cs1550_flush,
	.open	= cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
