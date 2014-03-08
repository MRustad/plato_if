/*
 * platomsg - Display message on PLATO terminal
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program in the file named COPYING.
 *
 * Copyright © 2014 Mark Rustad <MRustad@mac.com>
 *
 * Written by: Mark Rustad, 2014/03/01
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/spi/spidev.h>

#define HOST_DECODE 1

#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))
#define UNUSED	__attribute__((__unused__))

enum terminal_cmd_codes {
	CMD_NOP	= 0,	/* No-op */
	CMD_LDM = 1,	/* Load Mode */
	CMD_LDC = 2,	/* Load Coordinate */
	CMD_LDE = 3,	/* Load Echo */
	CMD_LDA = 4,	/* Load memory Address */
	CMD_SSL = 5,	/* Load Slide */
	CMD_AUD = 6,	/* Load Audio */
	CMD_EXT = 7	/* Load External Channel */
};

#define CMD_DEF(c,x) (((c) << 16) | (x))

static const uint32_t cmd_clear_screen =
			CMD_DEF(CMD_LDM, 033 << 1);

struct host_session {
	int		spi_fd;		/* SPI file descriptor */
	uint32_t	word_bits;	/* Accumulated data word bits */
	uint32_t	key_bits;	/* Accumulated keyset bits */
	uint16_t	key_bit_count;	/* Count of bits accumulated */
	bool		key_stop_search;
	int8_t		current_mem;	/* Current character memory */
	uint8_t		word_bit_count;	/* Count of bits in word_bits */
	uint8_t		spi_buf[6];
};

extern char *optarg;
extern int optind;
extern int optopt;
extern int opterr;
extern int optreset;

static int debug_flag;
static int clear_screen;

static const char *spi_dev = "/dev/spidev1.0";
static uint32_t	spi_speed = 5040;

static struct host_session sess = { .current_mem = -1 };

static const uint8_t a2p[256] = {
	[':'] = 0,	['a'] = 1,	['b'] = 2,	['c'] = 3,
	['d'] = 4,	['e'] = 5,	['f'] = 6,	['g'] = 7,
	['h'] = 8,	['i'] = 9,	['j'] = 10,	['k'] = 11,
	['l'] = 12,	['m'] = 13,	['n'] = 14,	['o'] = 15,
	['p'] = 16,	['q'] = 17,	['r'] = 18,	['s'] = 19,
	['t'] = 20,	['u'] = 21,	['v'] = 22,	['w'] = 23,
	['x'] = 24,	['y'] = 25,	['z'] = 26,	['0'] = 27,
	['1'] = 28,	['2'] = 29,	['3'] = 30,	['4'] = 31,
	['5'] = 32,	['6'] = 33,	['7'] = 34,	['8'] = 35,
	['9'] = 36,	['+'] = 37,	['-'] = 38,	['*'] = 39,
	['/'] = 40,	['('] = 41,	[')'] = 42,	['$'] = 43,
	['='] = 44,	[' '] = 45,	[','] = 46,	['.'] = 47,
			['%'] = 49,	['['] = 50,	[']'] = 51,
					['\''] = 54,	['"'] = 55,
	['!'] = 56,	[';'] = 57,	['<'] = 58,	['>'] = 59,
	['_'] = 60,	['?'] = 61,
	['#'] = 64,	['A'] = 65,	['B'] = 66,	['C'] = 67,
	['D'] = 68,	['E'] = 69,	['F'] = 70,	['G'] = 71,
	['H'] = 72,	['I'] = 73,	['J'] = 74,	['K'] = 75,
	['L'] = 76,	['M'] = 77,	['N'] = 78,	['O'] = 79,
	['P'] = 80,	['Q'] = 81,	['R'] = 82,	['S'] = 83,
	['T'] = 84,	['U'] = 85,	['V'] = 86,	['W'] = 87,
	['X'] = 88,	['Y'] = 89,	['Z'] = 90,
			['^'] = 93,
	['~'] = 100,
			['{'] = 105,	['}'] = 106,	['&'] = 107,
					['|'] = 110,
			['@'] = 125,	['\\'] = 126,
};

#define KEY_NEXT	026
#define KEY_STOP	032
#define KEY_STOP1	072
#define KEY_TURNON	01700
#define KEY_DATA	031
#define KEY_LC_A	0101
#define KEY_XON		01606
#define KEY_XOFF	01607

#define LC_KEY(x)	(x - 'a' + KEY_LC_A)

const char * const key_decode[02000] = {
	[KEY_NEXT] = "-next-",
	[KEY_DATA] = "-data-",
	[KEY_STOP] = "-stop-",
	[KEY_STOP1] = "-stop1-",
	[LC_KEY('a')] = "a", [LC_KEY('b')] = "b", [LC_KEY('c')] = "c",
	[LC_KEY('d')] = "d", [LC_KEY('e')] = "e", [LC_KEY('f')] = "f",
	[LC_KEY('g')] = "g", [LC_KEY('h')] = "h", [LC_KEY('i')] = "i",
	[LC_KEY('j')] = "j", [LC_KEY('k')] = "k", [LC_KEY('l')] = "l",
	[LC_KEY('m')] = "m", [LC_KEY('n')] = "n", [LC_KEY('o')] = "o",
	[LC_KEY('p')] = "p", [LC_KEY('q')] = "q", [LC_KEY('r')] = "r",
	[LC_KEY('s')] = "s", [LC_KEY('t')] = "t", [LC_KEY('u')] = "u",
	[LC_KEY('v')] = "v", [LC_KEY('w')] = "w", [LC_KEY('x')] = "x",
	[LC_KEY('y')] = "y", [LC_KEY('z')] = "z",
	[KEY_XON] = "-flowon-",
	[KEY_XOFF] = "-flowoff-",
	[KEY_TURNON] = "-turnon-",
};

#if HOST_DECODE
/**
 * chmem - Return possible characters
 *
 * @ch: Six-bit character
 *
 * Returns pointer to string of possible characters
 */
static const char *chmem(uint8_t ch)
{
        static const char * const strs[] = {
                ":#", "aA", "bB", "cC", "dD", "eE", "fF", "gG",
                "hH", "iI", "jJ", "kK", "lL", "mM", "nN", "oO",
                "pP", "qQ", "rR", "sS", "tT", "uU", "vV", "wW",
                "xX", "yY", "zZ", "0¨", "1\"", "2^", "3'", "4`",
                "5", "6", "7", "8", "9~", "+", "-", "*",
                "/", "({", ")}", "$&", "=/=", "  ", ",|", ".",
                "?", "[", "]", "%", "?", "<-µ", "'∏", "\"",
                "!", ";", "<", ">", "_", "?@", ">>\\", "uncover"
        };

        return strs[ch];
}

/**
 * decode_host_word - Decode host word
 * @w: Output word
 */
static void decode_host_word(uint32_t w)
{
	uint8_t cmd = (w >> 16) & 7;
	static const char *mstrs[8] = {
		"Erase", "Erase, Screen erase",
		"Rewrite", "rewrite, Screen Erase",
		"Erase", "Erase, Screen erase",
		"Write", "Write, Screen Erase"
	};

	if (w & (1 << 19)) {
		fprintf(stderr, "DW %07o\t%s\t%s\t%s\n",
			w, chmem((w >> 13) & 077),
			chmem((w >> 7) & 077), chmem((w >> 1) & 077));
		return;
	}

	fprintf(stderr, "CW %07o: ", w);

	switch (cmd) {
	case CMD_NOP:
		fprintf(stderr, "NOP\n");
		return;

	case CMD_LDM:
		fprintf(stderr, "LDM I=%d, ", (w >> 15) & 1);
		if ((w >> 14) & 1)
			fprintf(stderr, "wc=%d, ", (w >> 7) & 0177);
		fprintf(stderr, "mode=%d, %s\n", (w >> 4) & 03, mstrs[(w >> 1) & 07]);
		return;

	case CMD_LDC:
		fprintf(stderr, "LDC %c=%d\n", (w & (1 << 10)) ? 'Y' : 'X',
			(w >> 1) & 0777);
		return;

	case CMD_LDE:
		fprintf(stderr, "LDE %d (%04o)\n", (w >> 1) & 0177,
			(w >> 1) & 0177);
		return;

	case CMD_LDA:
		fprintf(stderr, "LDA %d (%04o)\n", (w >> 1) & 01777,
			(w >> 1) & 01777);
		return;

	case CMD_SSL:
		fprintf(stderr, "SSL L=%d, S=%d, X=%d, Y=%d\n", (w >> 10) & 1,
			(w >> 9) & 1, (w >> 5) & 017, (w >> 1) & 017);
		return;

	case CMD_AUD:
		fprintf(stderr, "AUD %d (%05o)\n", (w >> 1) & 077777,
			(w >> 1) & 077777);
		return;

	case CMD_EXT:
		fprintf(stderr, "EXT %d (%05o)\n", (w >> 1) & 077777,
			(w >> 1) & 077777);
		return;

	default:
		fprintf(stderr, "Unknown command: %d\n", cmd);
	}
}
#endif /* HOST_DECODE */

/**
 * send_word() - Send word to terminal
 * @sess: Pointer to host_session
 * @word: Word to send to terminal
 */
static void send_word(struct host_session *sess, uint32_t word)
{
	uint8_t bytes[sizeof(sess->spi_buf)];
	struct spi_ioc_transfer spi_xfer = {
		.tx_buf = (__u64)(unsigned long)bytes,
		.rx_buf = (__u64)(unsigned long)sess->spi_buf,
		.len = sizeof(sess->spi_buf),
		.delay_usecs = 0,
		.speed_hz = spi_speed,
		.bits_per_word = 8,
	};
	unsigned int i;
	int rc;

#if HOST_DECODE
	decode_host_word(word);
#endif /* HOST_DECODE */
	word <<= 11;
	bytes[0] = word >> 24;
	bytes[1] = word >> 16;
	bytes[2] = word >> 8;
	for (i = 3; i < sizeof(bytes); ++i)
		bytes[i] = 0;
	fprintf(stderr, "%02x %02x %02x\n", bytes[0], bytes[1], bytes[2]);
	rc = ioctl(sess->spi_fd, SPI_IOC_MESSAGE(1), &spi_xfer);
	if (rc < 0) {
		fprintf(stderr, "%s: write error: %m\n", __func__);
		return;
	}
	usleep(10000);		/* Delay */
}

#if 0
static uint32_t fls(uint32_t w)
{
	uint32_t prev;

	if (!w)
		return 0;
	while (w) {
		prev = w;
		w &= w - 1;
	}
	return ffs(prev);
}

static void process_spi_byte(struct host_session *sess, uint8_t byte)
{
	if (sess->key_bit_count == 0) {
		if (sess->key_stop_search) {
			if (byte == 0xff)
				return;
			sess->key_stop_search = false;
		}
		if (byte == 0)
			return;
		sess->key_bit_count = fls(byte);
		sess->key_bits = byte;
		return;
	}
	sess->key_bits = (sess->key_bits << 8) | byte;
	sess->key_bit_count += 8;
	if (sess->key_bit_count >= 12) {
		uint16_t key_data;
		int bits_remaining = sess->key_bit_count - 12;

		key_data = sess->key_bits >> bits_remaining;
		key_data = (key_data >> 1) & 0x3ff;
		send_key(sess, key_data);
		sess->key_stop_search = true;
		if (key_data == KEY_STOP || key_data == KEY_STOP1)
			abort_all_output(sess);
		sess->key_bits &= (1 << bits_remaining) - 1;
		sess->key_bit_count -= 12;
		if (sess->key_bit_count > 0) {
			if (sess->key_bits != (1U << sess->key_bit_count) - 1) {
				sess->key_stop_search = false;
				sess->key_bit_count = fls(sess->key_bits);
				return;
			}
		}
		sess->key_bit_count = 0;
	}
}

static void process_spi_input(struct host_session *sess)
{
	unsigned int i;
	uint8_t	*bytes = sess->spi_buf;

	for (i = 0; i < sizeof(sess->spi_buf); ++i)
		process_spi_byte(sess, bytes[i]);

	return;
}
#endif /* 0 */

/* host_word_parity - Compute host word parity
 * @w: 19-bit word to compute parity on
 *
 * Return 1 if parity was odd, 0 if it was even
 */
static uint32_t	host_word_parity(uint32_t w)
{
	static const uint32_t	p32 = 0x96696996;
	uint32_t	parity = 0;

	while (w) {
		uint8_t	bits = w & 037;		/* Extract 5 bits */

		parity ^= (p32 >> bits) & 1;
		w >>= 5;
	}

	return parity;
}

/**
 * make_word - Make host word by adding start bit and parity
 * @word: Host data
 *
 * Returns host word
 */
static uint32_t make_word(uint32_t word)
{
	word |= host_word_parity(word);
	word |= 1 << 20;
	return word;
}

/**
 * pack_tb - Pack text mode bytes
 * @sess: Pointer to host_session
 * @tb: Text byte (6-bit value)
 */
static void pack_tb(struct host_session *sess, uint8_t tb)
{
	sess->word_bits = (sess->word_bits << 6) | (tb & 077);
	sess->word_bit_count += 6;
	if (sess->word_bit_count < 18)
		return;
	sess->word_bits <<= 1;
	sess->word_bits |= 1 << 19;
	send_word(sess, make_word(sess->word_bits));
	sess->word_bits = 0;
	sess->word_bit_count = 0;
}

/**
 * flush_data - Flush accumulated data to terminal
 * sess: Pointer to host_session
 */
static void flush_data(struct host_session *sess)
{
	switch (sess->word_bit_count) {
	case 12:
		pack_tb(sess, 077);
		pack_tb(sess, 077);
		/* Fall through */
	case 6:
		pack_tb(sess, 077);
		pack_tb(sess, 020 + sess->current_mem);
	case 0:
		return;
	default:
		fprintf(stderr, "Unexpected bit count = %d\n",
			sess->word_bit_count);
	}
}

/**
 * send_text - Convert and send ASCII text to terminal
 * @sess: Pointer to host_session
 * @abp: Pointer to bytes to send
 */
static void send_text(struct host_session *sess, const uint8_t *abp)
{
	for (; *abp; ++abp) {
		uint8_t pb = a2p[*abp];
		uint8_t mem = (pb >> 6) & 3;

		if (mem != sess->current_mem) {
			pack_tb(sess, 077);
			pack_tb(sess, 020 + mem);
			sess->current_mem = mem;
		}
		pack_tb(sess, pb & 077);
	}
}

/**
 * usage - Print command usage information
 */
static void usage(const char *cmd)
{
	fprintf(stderr, "%s: Command usage:\n", cmd);
	fprintf(stderr,
		"\t-c\tClear screen\n"
		"\t-d\tEnable debugging\n"
		"\t-h\tDisplay this help\n"
		"\t-r\tSPI rate\n"
		"\t-s\tSPI device path\n");
}

/**
 * process_arguments - Process arguments
 * @argc: Number of arguments
 * @argv: Pointer to an array of pointers to arguments
 *
 * Return 0 if success, non-zero on some error
 */
static int process_arguments(int argc, char *argv[])
{
	int ch;
	const char *cmd = argv[0];

	while ((ch = getopt(argc, argv, "cdhr:s:")) != -1) {
		switch (ch) {
		case 'c':
			++clear_screen;
			break;
		case 'd':
			++debug_flag;
			break;
		case 'h':
			usage(cmd);
			exit(0);
		case 'r':
			spi_speed = atoi(optarg);
			break;
		case 's':
			spi_dev = optarg;
			break;
		case '?':
		default:
			return 2;
		}
	}

	argc -= optind;
	argv += optind;

	return 0;
}

/**
 * open_spi - Open spi device
 * @dev: Path to device
 * @speed: Maximum speed
 *
 * Returns file desriptor or -1 if failed
 */
static int open_spi(const char *dev, uint32_t speed)
{
	int fd;
	int rc;
	uint8_t mode;
	uint8_t bits;

	mode = SPI_MODE_1;
	bits = 8;
	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		int err = errno;

		fprintf(stderr, "Failed to open SPI device %s, errno=%d\n",
			dev, err);
		errno = err;
		return -1;
	}

	/* Set SPI mode */

	rc = ioctl(fd, SPI_IOC_WR_MODE, &mode);
	if (rc == -1) {
		fprintf(stderr, "SPI_IOC_WR_MODE failed, errno=%d\n", errno);
		goto err_exit;
	}

	rc = ioctl(fd, SPI_IOC_RD_MODE, &mode);
	if (rc == -1) {
		fprintf(stderr, "SPI_IOC_RD_MODE failed, errno=%d\n", errno);
		goto err_exit;
	}

	/* SPI bits per word */

	rc = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
	if (rc == -1) {
		fprintf(stderr, "SPI_IOC_WR_BITS_PER_WORD failed, errno=%d\n",
			errno);
		goto err_exit;
	}

	rc = ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &bits);
	if (rc == -1) {
		fprintf(stderr, "SPI_IOC_RD_BITS_PER_WORD failed, errno=%d\n",
			errno);
		goto err_exit;
	}

	/* SPI speed */

	rc = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
	if (rc == -1) {
		fprintf(stderr, "SPI_IOC_WR_MAX_SPEED failed, errno=%d\n",
			errno);
		goto err_exit;
	}

	rc = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed);
	if (rc == -1) {
		fprintf(stderr, "SPI_IOC_RD_MAX_SPEED failed, errno=%d\n",
			errno);
		goto err_exit;
	}

	return fd;

err_exit:
	close(fd);
	return -1;
}

/**
 * main - Main program
 * @argc: Count of arguments passed
 * @argv: Pointer to an array of pointers to arguments
 *
 * Returns exit status
 */
int main(int argc, char *argv[])
{
	int rc;

	rc = process_arguments(argc, argv);
	if (rc) {
		usage(argv[0]);
		return rc;
	}

	argc -= optind;
	argv += optind;

	sess.spi_fd = open_spi(spi_dev, spi_speed);
	if (sess.spi_fd < 0) {
		int err = errno;

		fprintf(stderr, "Failed to open SPI device %s, errno=%d\n",
			spi_dev, err);
		exit(err);
	}
	fcntl(sess.spi_fd, F_SETFL, O_NONBLOCK);

	if (clear_screen) {
		send_word(&sess, make_word(cmd_clear_screen));
		pack_tb(&sess, 077);
		pack_tb(&sess, 014);
	}

	if (argc <= 0)
		return 0;
		
	for (; argc > 0; ++argv, --argc) {
		uint8_t *a = (uint8_t *)argv[0];

		send_text(&sess, a);
		if (argc > 1 && *argv[1])
			send_text(&sess, (uint8_t *)" ");
	}
	pack_tb(&sess, 077);
	pack_tb(&sess, 015);
	flush_data(&sess);

	return 0;
}
