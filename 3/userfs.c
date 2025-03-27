#include "userfs.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char *memory;
	/** How many bytes are occupied. */
	int occupied;
	/** Next block in the file. */
	struct block *next;
	/** Previous block in the file. */
	struct block *prev;

	/* PUT HERE OTHER MEMBERS */
};

struct file {
	/** Double-linked list of file blocks. */
	struct block *block_list;
	/**
	 * Last block in the list above for fast access to the end
	 * of file.
	 */
	struct block *last_block;
	/** How many file descriptors are opened on the file. */
	int refs;
	/** File name. */
	char *name;
	/** Files are stored in a double-linked list. */
	struct file *next;
	struct file *prev;
	
	/** Total size of the file. */
	size_t size;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
	struct file *file;
	
	/** Current block where the file descriptor is positioned. */
	struct block *block;
	/** Position inside the current block. */
	int block_pos;
	/** Absolute position in the file. */
	size_t pos;
	
#if NEED_OPEN_FLAGS
	/** Access mode for the file descriptor. */
	int mode;
#endif
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

int
ufs_open(const char *filename, int flags)
{
	/* Find an existing file or create a new one */
	struct file *file = NULL;
	struct file *iter = file_list;
	while (iter != NULL) {
		if (strcmp(iter->name, filename) == 0) {
			file = iter;
			break;
		}
		iter = iter->next;
	}

	/* No file was found */
	if (file == NULL) {
		/* Only create file if UFS_CREATE flag is set */
		if (!(flags & UFS_CREATE)) {
			ufs_error_code = UFS_ERR_NO_FILE;
			return -1;
		}
		
		/* Create a new file */
		file = (struct file *)malloc(sizeof(struct file));
		if (file == NULL) {
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}
		
		/* Initialize the file */
		file->name = strdup(filename);
		if (file->name == NULL) {
			free(file);
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}
		
		file->block_list = NULL;
		file->last_block = NULL;
		file->size = 0;
		file->refs = 0;
		
		/* Add to file list */
		file->next = file_list;
		file->prev = NULL;
		if (file_list != NULL) {
			file_list->prev = file;
		}
		file_list = file;
	}
	
#if NEED_OPEN_FLAGS
	int mode = UFS_READ_WRITE;
	if (flags & UFS_READ_ONLY) {
		mode = UFS_READ_ONLY;
	} else if (flags & UFS_WRITE_ONLY) {
		mode = UFS_WRITE_ONLY;
	}
#endif

	/* Create a file descriptor */
	struct filedesc *fd = (struct filedesc *)malloc(sizeof(struct filedesc));
	if (fd == NULL) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	fd->file = file;
	fd->block = file->block_list;
	fd->block_pos = 0;
	fd->pos = 0;
#if NEED_OPEN_FLAGS
	fd->mode = mode;
#endif

	/* Find an empty slot in descriptors array or expand it */
	int fd_index = -1;
	for (int i = 0; i < file_descriptor_count; i++) {
		if (file_descriptors[i] == NULL) {
			fd_index = i;
			break;
		}
	}
	
	if (fd_index == -1) {
		/* Expand the array if needed */
		if (file_descriptor_count == file_descriptor_capacity) {
			int new_capacity = file_descriptor_capacity == 0 ? 8 : file_descriptor_capacity * 2;
			struct filedesc **new_descriptors = 
				(struct filedesc **)realloc(file_descriptors, new_capacity * sizeof(struct filedesc *));
			if (new_descriptors == NULL) {
				free(fd);
				ufs_error_code = UFS_ERR_NO_MEM;
				return -1;
			}
			
			/* Initialize new slots to NULL */
			for (int i = file_descriptor_capacity; i < new_capacity; i++) {
				new_descriptors[i] = NULL;
			}
			
			file_descriptors = new_descriptors;
			file_descriptor_capacity = new_capacity;
		}
		
		fd_index = file_descriptor_count;
		file_descriptor_count++;
	}
	
	file_descriptors[fd_index] = fd;
	file->refs++;
	
	return fd_index;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	/* Check if the file descriptor is valid */
	if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	
	struct filedesc *fdesc = file_descriptors[fd];
	struct file *file = fdesc->file;
	
#if NEED_OPEN_FLAGS
	/* Check access mode */
	if (fdesc->mode == UFS_READ_ONLY) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	/* Don't do anything for a zero-sized write */
	if (size == 0) {
		return 0;
	}
	
	/* Check against maximum file size */
	if (fdesc->pos >= MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	
	if (fdesc->pos + size > MAX_FILE_SIZE) {
		size = MAX_FILE_SIZE - fdesc->pos;
	}
	
	/* Find the starting block and position based on fd->pos */
	size_t current_pos = 0;
	struct block *current_block = file->block_list;
	int block_pos = 0;
	
	/* Find the correct block for our current position */
	while (current_block != NULL) {
		size_t block_size = current_block->occupied;
		if (current_pos + block_size > fdesc->pos) {
			/* We found the block containing our position */
			block_pos = fdesc->pos - current_pos;
			break;
		}
		current_pos += block_size;
		current_block = current_block->next;
	}
	
	/* If we're beyond the end of the file, or file is empty, create new blocks */
	if (current_block == NULL) {
		/* If file has blocks but we're at the end */
		if (file->last_block != NULL) {
			current_block = file->last_block;
			block_pos = current_block->occupied;
		} else {
			/* File is empty, create first block */
			current_block = (struct block *)malloc(sizeof(struct block));
			if (current_block == NULL) {
				ufs_error_code = UFS_ERR_NO_MEM;
				return -1;
			}
			
			current_block->memory = (char *)malloc(BLOCK_SIZE);
			if (current_block->memory == NULL) {
				free(current_block);
				ufs_error_code = UFS_ERR_NO_MEM;
				return -1;
			}
			
			current_block->occupied = 0;
			current_block->next = NULL;
			current_block->prev = NULL;
			
			file->block_list = current_block;
			file->last_block = current_block;
			
			block_pos = 0;
		}
	}
	
	size_t bytes_left = size;
	size_t buf_offset = 0;
	
	while (bytes_left > 0) {
		/* If we're at the end of the current block, move to the next or create a new one */
		if (block_pos >= BLOCK_SIZE) {
			if (current_block->next == NULL) {
				/* Create a new block */
				struct block *new_block = (struct block *)malloc(sizeof(struct block));
				if (new_block == NULL) {
					ufs_error_code = UFS_ERR_NO_MEM;
					return size - bytes_left;
				}
				
				new_block->memory = (char *)malloc(BLOCK_SIZE);
				if (new_block->memory == NULL) {
					free(new_block);
					ufs_error_code = UFS_ERR_NO_MEM;
					return size - bytes_left;
				}
				
				new_block->occupied = 0;
				new_block->next = NULL;
				new_block->prev = current_block;
				
				current_block->next = new_block;
				file->last_block = new_block;
			}
			
			current_block = current_block->next;
			block_pos = 0;
		}
		
		/* Calculate how much we can write in this block */
		size_t block_space = BLOCK_SIZE - block_pos;
		size_t to_write = bytes_left < block_space ? bytes_left : block_space;
		
		/* Copy data to the block */
		memcpy(current_block->memory + block_pos, buf + buf_offset, to_write);
		
		/* Update positions and counters */
		block_pos += to_write;
		buf_offset += to_write;
		bytes_left -= to_write;
		
		/* Update block occupation if we wrote beyond current occupation */
		if (block_pos > current_block->occupied) {
			current_block->occupied = block_pos;
		}
	}
	
	/* Update file descriptor position */
	fdesc->block = current_block;
	fdesc->block_pos = block_pos;
	fdesc->pos += size;
	
	/* Update file size if needed */
	if (fdesc->pos > file->size) {
		file->size = fdesc->pos;
	}
	
	return size;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	/* Check if the file descriptor is valid */
	if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	
	struct filedesc *fdesc = file_descriptors[fd];
	struct file *file = fdesc->file;
	
#if NEED_OPEN_FLAGS
	/* Check access mode */
	if (fdesc->mode == UFS_WRITE_ONLY) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	/* Check if we're at the end of the file */
	if (fdesc->pos >= file->size) {
		return 0; /* EOF */
	}
	
	/* Adjust size if it exceeds file boundaries */
	if (fdesc->pos + size > file->size) {
		size = file->size - fdesc->pos;
	}
	
	/* Handle empty files */
	if (file->block_list == NULL) {
		return 0;
	}
	
	/* Find the correct block and position based on fd->pos */
	size_t current_pos = 0;
	struct block *current_block = file->block_list;
	int block_pos = 0;
	
	/* Find the correct block for our current position */
	while (current_block != NULL) {
		size_t block_size = current_block->occupied;
		if (current_pos + block_size > fdesc->pos) {
			/* We found the block containing our position */
			block_pos = fdesc->pos - current_pos;
			break;
		}
		current_pos += block_size;
		current_block = current_block->next;
	}
	
	/* If we've reached the end of the file */
	if (current_block == NULL) {
		return 0;
	}
	
	size_t bytes_left = size;
	size_t buf_offset = 0;
	
	while (bytes_left > 0 && current_block != NULL) {
		/* Calculate how much we can read in this block */
		size_t bytes_available = current_block->occupied - block_pos;
		size_t to_read = bytes_left < bytes_available ? bytes_left : bytes_available;
		
		/* Copy data from the block to the buffer */
		memcpy(buf + buf_offset, current_block->memory + block_pos, to_read);
		
		/* Update positions and counters */
		block_pos += to_read;
		buf_offset += to_read;
		bytes_left -= to_read;
		
		/* If we've read all data in this block, move to the next one */
		if (block_pos >= current_block->occupied) {
			current_block = current_block->next;
			block_pos = 0;
		}
	}
	
	/* Update file descriptor position */
	fdesc->block = current_block;
	fdesc->block_pos = block_pos;
	fdesc->pos += (size - bytes_left);
	
	return size - bytes_left;
}

int
ufs_close(int fd)
{
	/* Check if the file descriptor is valid */
	if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	
	struct filedesc *fdesc = file_descriptors[fd];
	struct file *file = fdesc->file;
	
	/* Decrease file's reference count */
	file->refs--;
	
	/* Free file descriptor and clear the slot */
	free(fdesc);
	file_descriptors[fd] = NULL;
	
	return 0;
}

int
ufs_delete(const char *filename)
{
	/* Find the file by name */
	struct file *file = NULL;
	struct file *iter = file_list;
	while (iter != NULL) {
		if (strcmp(iter->name, filename) == 0) {
			file = iter;
			break;
		}
		iter = iter->next;
	}
	
	/* Check if the file exists */
	if (file == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	
	/* Remove the file from the linked list */
	if (file->prev != NULL) {
		file->prev->next = file->next;
	} else {
		file_list = file->next;
	}
	
	if (file->next != NULL) {
		file->next->prev = file->prev;
	}
	
	/* If there are open descriptors, just unlink but don't free yet */
	if (file->refs > 0) {
		return 0;
	}
	
	/* Free all blocks */
	struct block *block = file->block_list;
	while (block != NULL) {
		struct block *next = block->next;
		free(block->memory);
		free(block);
		block = next;
	}
	
	/* Free file name and file structure */
	free(file->name);
	free(file);
	
	return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
	/* Check if the file descriptor is valid */
	if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	
	struct filedesc *fdesc = file_descriptors[fd];
	struct file *file = fdesc->file;
	
#if NEED_OPEN_FLAGS
	/* Check if the descriptor has write permission */
	if (fdesc->mode == UFS_READ_ONLY) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif
	
	/* If new size is the same as current size, nothing to do */
	if (new_size == file->size) {
		return 0;
	}
	
	/* If shrinking the file */
	if (new_size < file->size) {
		size_t current_size = 0;
		struct block *block = file->block_list;
		struct block *last_needed_block = NULL;
		int last_block_occupied = 0;
		
		/* Find the block and position corresponding to the new size */
		while (block != NULL && current_size < new_size) {
			if (current_size + block->occupied >= new_size) {
				/* This is the last block we need */
				last_needed_block = block;
				last_block_occupied = new_size - current_size;
				break;
			}
			
			current_size += block->occupied;
			block = block->next;
		}
		
		/* If we're deleting all blocks */
		if (last_needed_block == NULL) {
			/* Free all blocks */
			block = file->block_list;
			while (block != NULL) {
				struct block *next = block->next;
				free(block->memory);
				free(block);
				block = next;
			}
			
			file->block_list = NULL;
			file->last_block = NULL;
		} else {
			/* Set the new last block */
			last_needed_block->occupied = last_block_occupied;
			
			/* Free all blocks after the new last block */
			block = last_needed_block->next;
			last_needed_block->next = NULL;
			file->last_block = last_needed_block;
			
			while (block != NULL) {
				struct block *next = block->next;
				free(block->memory);
				free(block);
				block = next;
			}
		}
		
		/* Update file size */
		file->size = new_size;
		
		/* Update all file descriptors that point past the new end */
		for (int i = 0; i < file_descriptor_count; i++) {
			struct filedesc *fd = file_descriptors[i];
			if (fd != NULL && fd->file == file && fd->pos > new_size) {
				fd->pos = new_size;
				fd->block = file->last_block;
				fd->block_pos = fd->block ? fd->block->occupied : 0;
			}
		}
		
		return 0;
	}
	
	/* If growing the file, we need to add blocks */
	size_t size_to_add = new_size - file->size;
	
	/* If the file is empty, create the first block */
	if (file->block_list == NULL) {
		file->block_list = (struct block *)malloc(sizeof(struct block));
		if (file->block_list == NULL) {
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}
		
		file->block_list->memory = (char *)malloc(BLOCK_SIZE);
		if (file->block_list->memory == NULL) {
			free(file->block_list);
			file->block_list = NULL;
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}
		
		file->block_list->occupied = 0;
		file->block_list->next = NULL;
		file->block_list->prev = NULL;
		
		file->last_block = file->block_list;
	}
	
	/* Fill the last block if it's not full */
	struct block *current_block = file->last_block;
	size_t space_in_last_block = BLOCK_SIZE - current_block->occupied;
	
	if (space_in_last_block > 0) {
		size_t fill_size = space_in_last_block < size_to_add ? space_in_last_block : size_to_add;
		/* Initialize new space with zeros */
		memset(current_block->memory + current_block->occupied, 0, fill_size);
		current_block->occupied += fill_size;
		size_to_add -= fill_size;
	}
	
	/* Add new blocks if needed */
	while (size_to_add > 0) {
		struct block *new_block = (struct block *)malloc(sizeof(struct block));
		if (new_block == NULL) {
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}
		
		new_block->memory = (char *)malloc(BLOCK_SIZE);
		if (new_block->memory == NULL) {
			free(new_block);
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}
		
		size_t block_size = size_to_add < BLOCK_SIZE ? size_to_add : BLOCK_SIZE;
		
		/* Initialize new block with zeros */
		memset(new_block->memory, 0, block_size);
		new_block->occupied = block_size;
		new_block->next = NULL;
		new_block->prev = current_block;
		
		current_block->next = new_block;
		file->last_block = new_block;
		
		current_block = new_block;
		size_to_add -= block_size;
	}
	
	/* Update file size */
	file->size = new_size;
	
	return 0;
}

#endif

void
ufs_destroy(void)
{
	/* Free all files and their blocks */
	struct file *file = file_list;
	while (file != NULL) {
		struct file *next_file = file->next;
		
		/* Free all blocks of this file */
		struct block *block = file->block_list;
		while (block != NULL) {
			struct block *next_block = block->next;
			free(block->memory);
			free(block);
			block = next_block;
		}
		
		free(file->name);
		free(file);
		file = next_file;
	}
	file_list = NULL;
	
	/* Free all file descriptors */
	for (int i = 0; i < file_descriptor_count; i++) {
		if (file_descriptors[i] != NULL) {
			free(file_descriptors[i]);
			file_descriptors[i] = NULL;
		}
	}
	
	free(file_descriptors);
	file_descriptors = NULL;
	file_descriptor_count = 0;
	file_descriptor_capacity = 0;
	
	/* Reset error code */
	ufs_error_code = UFS_ERR_NO_ERR;
}
