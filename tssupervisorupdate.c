#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <getopt.h>

#include "micro.h"
#include "update-v0.h"
#include "update-v1.h"

board_t boards[] = {
	{
		.compatible = "technologic,imx6q-ts7970",
		.i2c_bus = 0,
		.i2c_chip = 0x10,
		.modelnum = 0x7970,
		.method = UPDATE_V0,
	},
	{
		.compatible = "technologic,imx6dl-ts7970",
		.i2c_bus = 0,
		.i2c_chip = 0x10,
		.modelnum = 0x7970,
		.method = UPDATE_V0,
	},
	{
		.compatible = "technologic,ts7250v3",
		.i2c_bus = 0,
		.i2c_chip = 0x10,
		.modelnum = 0x7250,
		.method = UPDATE_V1,
	},
};

board_t *get_board()
{
	FILE *file;
	char comp[256];

	file = fopen("/sys/firmware/devicetree/base/compatible", "r");
	if (!file) {
		perror("Unable to open /sys/firmware/devicetree/base/compatible");
		return NULL;
	}

	if (fgets(comp, sizeof(comp), file) == NULL) {
		perror("Failed to read compatible string");
		fclose(file);
		return NULL;
	}
	fclose(file);

	for (int i = 0; i < (int)(sizeof(boards) / sizeof(boards[0])); i++) {
		if (strstr(comp, boards[i].compatible) != NULL) {
			return &boards[i];
		}
	}
	return NULL;
}

void usage(char **argv)
{
	fprintf(stderr,
		"Usage: %s [OPTION] ...\n"
		"embeddedTS supervisory microcontroller update utility\n"
		"\n"
		"  -i, --info             Print current revision information and close\n"
		"  -f, --force            Update even if revisions match (not recommended).\n"
		"                         Requires -u.\n"
		"  -n, --dry-run          Check file and current revision, prints the changes\n"
		"                         it would make but does not update.  Requires -u.\n"
		"  -u, --update <file>    Update file.\n"
		"  -b, --bus              Override default i2c bus\n"
		"  -c, --chip-addr        Override default i2c chip address\n"
		"  -h, --help             This message\n"
		"\n",
		argv[0]);
}

int main(int argc, char *argv[])
{
	int option_index = 0;
	board_t *board;
	int update_revision;
	int micro_revision;
	int i2cfd;
	int ret;
	int c;

	int dry_run_flag = 0;
	int force_flag = 0;
	int info_flag = 0;
	char *update_path = 0;
	int opt_bus = -1;
	int opt_chip_addr = -1;

	if (argc < 2) {
		usage(argv);
		return 1;
	}

	static struct option long_options[] = { { "info", no_argument, NULL, 'i' },
						{ "force", no_argument, NULL, 'f' },
						{ "update", required_argument, NULL, 'u' },
						{ "dry-run", no_argument, NULL, 'n' },
						{ "chip-addr", required_argument, NULL, 'c' },
						{ "bus", required_argument, NULL, 'b' },
						{ "help", no_argument, NULL, 'h' },
						{ 0, 0, 0, 0 } };

	while ((c = getopt_long(argc, argv, "u:nihfc:b:", long_options, &option_index)) != -1) {
		switch (c) {
		case 'f':
			force_flag = 1;
			break;
		case 'h':
			usage(argv);
			break;
		case 'i':
			info_flag = 1;
			break;
		case 'n':
			dry_run_flag = 1;
			break;
		case 'c':
			opt_chip_addr = strtoul(optarg, NULL, 0);
			break;
		case 'b':
			opt_bus = strtoul(optarg, NULL, 0);
			break;
		case 'u':
			update_path = optarg;
			break;
		case '?':
		default:
			printf("Unexpected argument \"%s\"\n", optarg);
			return 1;
		}
	}

	if ((dry_run_flag || force_flag) && update_path == NULL) {
		printf("Must specify the update file\n");
		return 1;
	}

	board = get_board();
	if (!board) {
		printf("Unsupported board\n");
		return 1;
	}

	if (opt_chip_addr != -1)
		board->i2c_chip = opt_chip_addr;

	if (opt_bus != -1)
		board->i2c_bus = opt_bus;

	int (*update_func)(board_t *board, int i2cfd, char *update_path) = NULL;
	int (*get_rev_func)(board_t *board, int i2cfd, int *revision) = NULL;
	int (*get_update_rev_func)(board_t *board, int *revision, char *update_path) = NULL;
	int (*print_micro_info_func)(board_t *board, int i2cfd) = NULL;

	switch (board->method) {
	case UPDATE_V0:
		update_func = do_v0_micro_update;
		get_rev_func = do_v0_micro_get_rev;
		get_update_rev_func = do_v0_micro_get_file_rev;
		print_micro_info_func = do_v0_micro_print_info;
		break;

	case UPDATE_V1:
		update_func = do_v1_micro_update;
		get_rev_func = do_v1_micro_get_rev;
		get_update_rev_func = do_v1_micro_get_file_rev;
		print_micro_info_func = do_v1_micro_print_info;
		break;

	default:
		printf("Unsupported update method\n");
		return -1;
	}

	i2cfd = micro_init(board->i2c_bus, board->i2c_chip);
	if (i2cfd < 0) {
		perror("i2c");
		return 1;
	}

	if (info_flag) {
		ret = print_micro_info_func(board, i2cfd);
		if (ret != 0)
			return ret;
	}

	if (update_path) {
		ret = get_rev_func(board, i2cfd, &micro_revision);
		if (ret != 0)
			return ret;

		ret = get_update_rev_func(board, &update_revision, update_path);
		if (ret != 0)
			return ret;

		if ((update_revision <= micro_revision) && !force_flag) {
			printf("Already at revision %d\n", update_revision);
		}

		printf("Updating from revision %d to %d\n", micro_revision, update_revision);

		if (dry_run_flag) {
			printf("Dry run specified, not updating\n");
			return 0;
		}

		ret = update_func(board, i2cfd, update_path);
		if (ret != 0)
			return ret;
	}

	close(i2cfd);

	return 0;
}
