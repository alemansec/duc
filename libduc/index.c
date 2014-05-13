
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>

#include "db.h"
#include "duc.h"
#include "duc-private.h"

#define OPEN_FLAGS (O_RDONLY | O_NOCTTY | O_DIRECTORY | O_NOFOLLOW)


struct index {
	struct duc *duc;
	int one_file_system;
	dev_t dev;
	size_t file_count;
	size_t dir_count;
	int depth;
};


off_t index_dir(struct index *index, const char *path, int fd_dir, struct stat *stat_dir)
{
	struct duc *duc = index->duc;
	off_t size_total = 0;

	int fd = openat(fd_dir, path, OPEN_FLAGS | O_NOATIME);

	if(fd == -1 && errno == EPERM) {
		fd = openat(fd_dir, path, OPEN_FLAGS);
	}

	if(fd == -1) {
		duc_log(duc, LG_WRN, "Skipping %s: %s\n", path, strerror(errno));
		return 0;
	}

	DIR *d = fdopendir(fd);
	if(d == NULL) {
		duc_log(duc, LG_WRN, "Skipping %s: %s\n", path, strerror(errno));
		return 0;
	}

	struct ducdir *dir = ducdir_new(index->duc, 8);
			
	if(index->dev == 0) {
		index->dev = stat_dir->st_dev;
	}

	struct dirent *e;
	while( (e = readdir(d)) != NULL) {

		/* Skip . and .. */

		const char *n = e->d_name;
		if(n[0] == '.') {
			if(n[1] == '\0') continue;
			if(n[1] == '.' && n[2] == '\0') continue;
		}

		/* Get file info */

		struct stat stat;
		int r = fstatat(fd, e->d_name, &stat, AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW);
		if(r == -1) {
			duc_log(duc, LG_WRN, "Error statting %s: %s\n", e->d_name, strerror(errno));
			continue;
		}

		/* Check for file system boundaries */

		if(index->one_file_system) {
			if(stat.st_dev != index->dev) {
				duc_log(duc, LG_WRN, "Skipping %s: different file system\n", e->d_name);
				continue;
			}
		}

		/* Calculate size, recursing when needed */

		off_t size = 0;
		
		if(S_ISDIR(stat.st_mode)) {
			index->depth ++;
			size = index_dir(index, e->d_name, fd, &stat);
			index->depth --;
			index->dir_count ++;
		} else {
			size = stat.st_size;
			index->file_count ++;
		}

		duc_log(duc, LG_DBG, "%s %jd\n", e->d_name, size);

		/* Store record */

		ducdir_add_ent(dir, e->d_name, size, stat.st_mode, stat.st_dev, stat.st_ino);
		size_total += size;
	}

	ducdir_write(dir, stat_dir->st_dev, stat_dir->st_ino);
	duc_closedir(dir);

	closedir(d);

	return size_total;
}	


int duc_index(duc *duc, const char *path, int flags)
{
	struct index index;
	memset(&index, 0, sizeof index);

	index.duc = duc;
	index.one_file_system = flags & DUC_INDEX_XDEV;

	char *path_canon = realpath(path, NULL);
	if(path_canon == NULL) {
		duc_log(duc, LG_WRN, "Error converting path %s: %s\n", path, strerror(errno));
		duc->err = DUC_E_UNKNOWN;
		if(errno == EACCES) duc->err = DUC_E_PERMISSION_DENIED;
		if(errno == ENOENT) duc->err = DUC_E_PATH_NOT_FOUND;
		return -1;
	}
	
	struct stat stat;
	int r = lstat(path_canon, &stat);
	if(r == -1) {
		duc_log(duc, LG_WRN, "Error statting %s: %s\n", path, strerror(errno));
		duc->err = DUC_E_UNKNOWN;
		if(errno == EACCES) duc->err = DUC_E_PERMISSION_DENIED;
		return -1;
	}

	duc_root_write(duc, path_canon, stat.st_dev, stat.st_ino);

	off_t size = index_dir(&index, path_canon, 0, &stat);

	free(path_canon);

	duc_log(duc, LG_INF, "Indexed %zu files and %zu directories, %jd bytes\n", 
			index.file_count, index.dir_count, size);

	return 0;
}


/*
 * End
 */
