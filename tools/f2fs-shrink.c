/*
 * Shrink a mounted f2fs in place (F2FS_IOC_RESIZE_FS, Linux 5.7+), for making room behind the U30 Pro's
 * userdata without erasing it: userdata is metadata-encrypted (dm-default-key), so it cannot be resized offline
 * from outside Android, but the kernel that has it mounted can move the segments out of the tail itself.
 *
 *   f2fs-shrink MOUNTPOINT BLOCKS     BLOCKS: the new size in 4 KiB f2fs blocks, a multiple of the section size
 *
 * The kernel checks first (free space, then free_segment_range as a dry run) and returns an error having changed
 * nothing; only after both pass does it freeze the filesystem, migrate the tail and commit a new superblock.
 * Build: $NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang -static -O2 -o f2fs-shrink f2fs-shrink.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define F2FS_IOCTL_MAGIC 0xf5
#define F2FS_IOC_RESIZE_FS _IOW(F2FS_IOCTL_MAGIC, 16, uint64_t)

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s MOUNTPOINT BLOCKS\n", argv[0]);
		return 2;
	}
	char *end;
	uint64_t blocks = strtoull(argv[2], &end, 10);
	if (*end || blocks == 0) {
		fprintf(stderr, "bad block count: %s\n", argv[2]);
		return 2;
	}
	int fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	if (ioctl(fd, F2FS_IOC_RESIZE_FS, &blocks) < 0) {
		fprintf(stderr, "F2FS_IOC_RESIZE_FS to %llu blocks: %s\n", (unsigned long long)blocks, strerror(errno));
		return 1;
	}
	printf("resized to %llu blocks\n", (unsigned long long)blocks);
	return 0;
}
