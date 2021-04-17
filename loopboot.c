#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <linux/loop.h>

#define TMPFS_MOUNTED (0x1 << 0)
#define DEVFS_MOUNTED (0x1 << 2)

#define DEFAULT_TMP _DEFAULT_TMP
static char *_DEFAULT_TMP = "/boottmp";

static FILE *log;

static int read_file(const char *path, char **out) {
	int fd = -1;
	char *content = NULL;
	size_t bytes_read = 0;

	if (path == NULL || out == NULL) {
		goto error;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(log, "Failed to open file (%s).\n", path);
		goto error;
	}

	struct stat file_stat;

	if (fstat(fd, &file_stat)) {
		fprintf(log, "Failed to fstat file (%s).\n", path);
		goto error;
	}

	size_t file_size = file_stat.st_size;
	size_t buffer_size = file_size + 1;
	content = malloc(buffer_size + 1);
	if (content == NULL) {
		goto error;
	}

	while (1) {
		if (buffer_size == bytes_read) {
			buffer_size = buffer_size * 2;
			content = realloc(content, buffer_size + 1);
		}
		int newly_read = read(fd, content + bytes_read, buffer_size - bytes_read);
		if (newly_read == 0) {
			break;
		}
		if (newly_read < 0) {
			fprintf(log, "Error while reading file (%s).\n", path);
			goto error;
		}
		bytes_read += newly_read;
	}

	close(fd);
	content[bytes_read] = 0;

	*out = content;
	return bytes_read;

	error:
		if (fd >- 0) {
			close(fd);
		}

		if (content != NULL) {
			free(content);
		}

		return -1;
}

static int create_loop(const char *file, const char *devroot) {
	int file_fd = open(file, O_RDWR);
	int dev_num = -1;
	int device_fd = -1; 
	int loopctl_fd = -1;

	size_t devroot_len = 0;
	devroot_len = strlen(devroot);

	char *loop_control_filename = NULL;
	char *loop_device_filename = NULL;

	struct loop_info64 info;

	if (file_fd < 0) {
		fprintf(log, "Failed to open backing file (%s).\n", file);
		goto error;
	}

	loop_control_filename = malloc(devroot_len + 32);
	sprintf(loop_control_filename, "%s/loop-control", devroot);
	loopctl_fd = open(loop_control_filename, O_RDWR);
	if (loopctl_fd < 0) {
		fprintf(log, "Failed to open loop-control.\n");
		goto error;
	}
	free(loop_control_filename);
	loop_control_filename = NULL;

	dev_num = ioctl(loopctl_fd, LOOP_CTL_GET_FREE);
	if (dev_num < 0) {
		fprintf(log, "Failed to find free loop device.\n");
		goto error;
	}

	if (dev_num >= 100) {
		fprintf(log, "dev number too large. \n");
		goto error;
	}

	loop_device_filename = malloc(devroot_len + 32);
	sprintf(loop_device_filename, "%s/loop%d", devroot, dev_num);
	if((device_fd = open(loop_device_filename, O_RDWR)) < 0) {
		fprintf(log, "Failed to open device (%s).\n", loop_device_filename);
		goto error;
	}
	free(loop_device_filename);
	loop_device_filename = NULL;

	if(ioctl(device_fd, LOOP_SET_FD, file_fd) < 0) {
		fprintf(log, "Failed to set fd.\n");
		goto error;
	}

	close(file_fd);
	file_fd = -1; 

	memset(&info, 0, sizeof(struct loop_info64));
	info.lo_offset = 0;
	if(ioctl(device_fd, LOOP_SET_STATUS64, &info)) {
		fprintf(log, "Failed to set info.\n");
		goto error;
	}

	close(device_fd);
	device_fd = -1;

	return dev_num;

	error:
		if (loop_control_filename != NULL) {
			free(loop_control_filename);
		}
		if (loop_device_filename != NULL) {
			free(loop_device_filename);
		}
		if(file_fd >= 0) {
			close(file_fd);
		}
		if(loopctl_fd >= 0) {
			close(loopctl_fd);
		}
		if(device_fd >= 0) {
			ioctl(device_fd, LOOP_CLR_FD, 0); 
			close(device_fd);
		}	 
		return -1;
}

static int destroy_loop(int dev_num, const char *devroot) {
	int device_fd = -1;
	char *device = NULL;
	device = malloc(strlen(devroot) + 32);
	if (device == NULL) {
		return -1;
	}
	sprintf(device, "%s/loop%d", devroot, dev_num);
	if((device_fd = open(device, O_RDWR)) < 0) {
		fprintf(log, "Failed to open device (%s).\n", device);
		free(device);
		return -1;
	}
	free(device);
	ioctl(device_fd, LOOP_CLR_FD, 0);
	close(device_fd);
	return 0;
}

typedef void (parse_cmdline_callback) (void *ctx, char *key, char *value);

static void parse_cmdline(char *cmdline, parse_cmdline_callback *cb, void *ctx) {
	size_t cmdline_len = strlen(cmdline);
	char *key = NULL;
	char *value = NULL;
	for (size_t i = 0; i <= cmdline_len; i++) {
		if (cmdline[i] == ' ' || cmdline[i] == '\n' || cmdline[i] == '\0') {
			cmdline[i] = '\0';
			if (key != NULL) {
				cb(ctx, key, value);
				key = NULL;
				value = NULL;
			}
		} else if (key == NULL && cmdline[i] != '=') {
			key = &cmdline[i];
		} else if (key != NULL && value == NULL && cmdline[i] == '=') {
			cmdline[i] = '\0';
			value = &cmdline[i+1];
		}
	}
}

struct parsed_cmdline {
	int authoritative;
	char *tmp;
	char *loop;
	char *loopfstype;
	char *loopflags;
	char *loopinit;
};

void cmdline_parser(void *ctx, char *key, char *value) {
	struct parsed_cmdline *parsed_cmdline = ctx;
	fprintf(log, "key=%s, value=%s\n", key, value);
	if (strcmp("loop", key) == 0) {
		parsed_cmdline->loop = value;
	} else if (strcmp("loopfstype", key) == 0) {
		parsed_cmdline->loopfstype = value;
	} else if (strcmp("loopflags", key) == 0) {
		parsed_cmdline->loopflags = value;
	} else if (strcmp("authoritative", key) == 0) {
		parsed_cmdline->authoritative = 1;
	} else if (strcmp("tmp", key) == 0) {
		if ((parsed_cmdline->tmp == DEFAULT_TMP || parsed_cmdline->tmp == NULL) && value != NULL) {
			parsed_cmdline->tmp = value;
		}
	} else if (strcmp("loopinit", key) == 0) {
		parsed_cmdline->loopinit = value;
	}
}

static int
pivot_root(const char *new_root, const char *put_old)
{
	return syscall(SYS_pivot_root, new_root, put_old);
}

static void
redirect_log(const char *file, const char *mode)
{
	log = fopen(file, mode);
	if (log == NULL) {
		log = stderr;
	}
}

static void
close_log()
{
	if (log == stderr) {
		return;
	}
	fflush(log);
	int log_fd = fileno(log);
	if (log_fd >= 0) {
		fsync(log_fd);
	}
	fclose(log);
	log = stderr;
}

static void
ls(const char *path)
{
	DIR *dirp = opendir(path);
	struct dirent *direntp = NULL;
	while ((direntp = readdir(dirp)) != NULL) {
		fprintf(log, "%s\n", direntp->d_name);
	}
	closedir(dirp);
}

int main(int argc, char *argv[], char *envp[]) {
	char *config_filename = NULL;
	char *log_filename = NULL;
	char *kernel_cmdline = NULL; int kernel_cmdline_len = -1;
	char *config_cmdline = NULL; int config_cmdline_len = -1;

	char *proc_mount = NULL;
	char *proc_cmdline_filename = NULL;
	char *devroot = NULL;
	char *loop_device = NULL;
	char *root = NULL;

	int loop_dev_num = -1;
	
	int tmpfs_mount_res = -1;
	int devfs_mount_res = -1;

	log = stderr;

	size_t tmp_path_len = 0;
	struct parsed_cmdline parsed_cmdline;
	memset(&parsed_cmdline, 0, sizeof(parsed_cmdline));

	parsed_cmdline.tmp = DEFAULT_TMP;
	tmp_path_len = strlen(parsed_cmdline.tmp);

	if (argc < 1) {
		fprintf(log, "argc is less than 1\n");
		goto error;
	}

	log_filename = malloc(strlen(argv[0]) + 4);
	if (log_filename == NULL) {
		goto error;
	}
	sprintf(log_filename, "%s.log", argv[0]);
	redirect_log(log_filename, "w");
	free(log_filename);
	log_filename = NULL;

	// read config file
	config_filename = malloc(strlen(argv[0]) + 5);
	if (config_filename == NULL) {
		goto error;
	}
	sprintf(config_filename, "%s.conf", argv[0]);
	config_cmdline_len = read_file(config_filename, &config_cmdline);
	free(config_filename);
	config_filename = NULL;
	if (config_cmdline_len > 0) {
		parse_cmdline(config_cmdline, cmdline_parser, &parsed_cmdline);
	}
	tmp_path_len = strlen(parsed_cmdline.tmp);

	tmpfs_mount_res = mount("tmpfs", parsed_cmdline.tmp, "tmpfs", 0, "size=64k,uid=0,gid=0,mode=700");
	if (tmpfs_mount_res != 0) {
		fprintf(log, "Cannot create tmpfs\n");
		goto error;
	}

	if (parsed_cmdline.loop == NULL ||
		parsed_cmdline.loopflags == NULL ||
		parsed_cmdline.loopfstype == NULL) {
		fprintf(log, "cmdline arguments incomplete\n");
		goto error;
	}

	devroot = malloc(tmp_path_len + 32);
	if (devroot == NULL) {
		goto error;
	}
	sprintf(devroot, "%s/dev", parsed_cmdline.tmp);
	mkdir(devroot, 0700);
	devfs_mount_res = mount("devtmpfs", devroot, "devtmpfs", 0, "");
	if (devfs_mount_res != 0) {
		fprintf(log, "cannot mount devfs\n");
		goto error;
	}

	loop_dev_num = create_loop(parsed_cmdline.loop, devroot);
	if (loop_dev_num < 0) {
		fprintf(log, "cannot setup loop device\n");
		goto error;
	}

	loop_device = malloc(tmp_path_len + 32);
	sprintf(loop_device, "%s/loop%d", devroot, loop_dev_num);

	root = malloc(tmp_path_len + 32);
	sprintf(root, "%s/root", parsed_cmdline.tmp);

	mkdir(root, 0700);
	if (mount(loop_device, root, parsed_cmdline.loopfstype, 0, parsed_cmdline.loopflags) != 0) {
		fprintf(log, "cannot setup loop device mount\n");
		goto error;
	}

	fprintf(log, "loop file mounted ok. pivoting root.\n");

	// pivot_root
	chdir(root);
	if (pivot_root(".", ".") == -1) {
		perror("pivot_root failed");
		fprintf(log, "Cannot pivot_root\n");
		goto error;
	}
	umount2(".", MNT_DETACH);

	chdir("/");

	fprintf(log, "pivot_root ok.\n");
	fprintf(log, "listing new root.\n");
	ls("/");

	free(devroot);
	free(root);
	free(loop_device);

	if (parsed_cmdline.loopinit) {
		fprintf(log, "about to execute init\n");
		argv[0] = parsed_cmdline.loopinit;
		for (int i = 0; argv[i] != NULL; i++) {
			fprintf(log, "argv[%d] = %s\n", i, argv[i]);
		}
		for (int i = 0; envp[i] != NULL; i++) {
			fprintf(log, "envp[%d] = %s\n", i, envp[i]);
		}
	}

	if (log != stderr) {
		close_log();
	}

	if (parsed_cmdline.loopinit != NULL) {
		execve(parsed_cmdline.loopinit, argv, envp);
	}

	return 0;

	error:
		if (loop_dev_num >= 0) {
			destroy_loop(loop_dev_num, devroot);
		}
		if (devfs_mount_res == 0) {
			umount(devroot);
		}
		if (tmpfs_mount_res == 0) {
			umount(parsed_cmdline.tmp);
		}

		if (log != stderr) {
			close_log();
		}

		if (log_filename != NULL) {
			free(log_filename);
		}
		if (config_filename != NULL) {
			free(config_filename);
		}
		if (kernel_cmdline != NULL) {
			free(kernel_cmdline);
		}
		if (config_cmdline != NULL) {
			free(config_cmdline);
		}
		if (proc_mount != NULL) {
			free(proc_mount);
		}
		if (devroot != NULL) {
			free(devroot);
		}
		if (loop_device != NULL) {
			free(loop_device);
		}
		if (root != NULL) {
			free(root);
		}
		return -1;
}
