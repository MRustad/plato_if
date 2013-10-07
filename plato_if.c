/*
 * plato_if - An interface for PLATO terminals
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright © 2013 Mark Rustad <MRustad@mac.com>
 *
 * Written by: Mark Rustad, 2013/01/20
 *
 * This program is used to interface PLATO IV terminals to the
 * Cyber1 system on the internet.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <alsa/asoundlib.h>
#include <linux/spi/spidev.h>

#define NO_TERMINAL	0
#define HOST_DECODE1	0
#define HOST_DECODE2	1

#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))
#define UNUSED	__attribute__((__unused__))

#define GSW_CRYSTAL	3872000	/* GSW clock frequency */

bool	audio_opened;

#define SND_RATE	24000	/* Sound sample rate */
#define SND_PERIODS	2
#define SND_CHANNELS	2
#define FRAME_SIZE	(sizeof(int16_t) * SND_CHANNELS)
#define FRAMES_PER_PERIOD	(SND_RATE / 60)
#define PERIOD_SIZE	(FRAMES_PER_PERIOD * FRAME_SIZE)
#define SND_BUFFER_SIZE	(FRAMES_PER_PERIOD * SND_PERIODS)
#define NVSHIFT		2
#define VOICES		(1 << NVSHIFT)
#define PHASEINCR	((GSW_CRYSTAL + SND_RATE - 1) / SND_RATE)

/* GSW frequency calculation
 * ext(x) = (crystal/x-2)/4
 *
 * freq(x) = (crystal/2)/(2*n+1)
 * freq(x) = (crystal)/(4*n+2)
 * freq(x) = crystal/(4*n+2)
 */
#define F2E(f)	((GSW_CRYSTAL / (f) - 2) / 4)
#define E2D(e)	((e) * 4 + 2)

struct wave {
	const int16_t	*samples;
	uint8_t		nsamp;
};

#define WAVEDEF(n, ...) static const int16_t _##n##_v_[] = { __VA_ARGS__ }; \
	const struct wave n = { _##n##_v_, ARRAY_SIZE(_##n##_v_) }

WAVEDEF(sq, 0x7FFF, 0);

static const int16_t silence[FRAMES_PER_PERIOD * SND_CHANNELS];

struct voice {
	uint32_t	div;
	uint32_t	frac;
	uint16_t	shift;
	uint16_t	step;
	uint16_t	phase;
	const struct amp	*amp;
	struct wave	wave;
};

struct amp {
	const uint16_t	mult;
	const uint8_t	shift;
};

static const struct amp amp[8] = {
	{ 2187, 14 },	{ 729, 12 },	{ 243, 10 },	{ 81, 8 },
	{ 27, 6},	{ 9, 4 },	{ 3, 2 },	{ 1, 0 }
};

/* frac_gen - Generate fraction number to avoid division
 * @div: Desired divisor
 * @shift: Pointer to uint16_t to receive shift count
 *
 * Returns fractional value
 */
static uint32_t frac_gen(uint32_t div, uint16_t *shift)
{
	uint32_t l32;
	uint32_t bit;

	l32 = 1 << 30;
	l32 /= div;
	for (bit = 29; ((1 << bit) & l32) == 0; --bit)
		;
	if (bit > 15) {
		l32 += 1 << (bit - 16);
		l32 >>= bit - 15;
		*shift = 16 + 29 - bit;
	} else
		*shift = 30;
	return l32;
}

/* generate - Generate next sample for a voice
 * @v: Pointer to voice structure
 */
static unsigned int generate(struct voice *v)
{
	uint32_t product;
	uint16_t ix;

	if (v->div < PHASEINCR)
		return 0;

	v->phase += PHASEINCR;
	while (v->phase >= v->div)
		v->phase -= v->div;

	product = v->phase * v->frac;
	product >>= v->shift;
	ix = product;
	if (ix >= v->wave.nsamp)
		ix = v->wave.nsamp - 1;

	return (v->amp->mult * v->wave.samples[ix]) >> v->amp->shift;
}

#define	HOST_IN_WORDS	5000
#define LDE_WAIT	5

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

enum host_states { in_sync, out_of_sync };

struct host_session {
	int		fd;		/* File descriptor for session */
	int		spi_fd;		/* SPI file descriptor */
	enum host_states host_state;
	uint16_t	erase_abort_count;
	uint16_t	inwd_in;
	uint16_t	inwd_out;
	uint32_t	inwds[HOST_IN_WORDS];
	int32_t		pending_echo;
	uint32_t	gsw_words[32];
	uint32_t	gsw_cnt;
	uint8_t		current_mode;
	uint8_t		cis;		/* GSW count inhibit */
	uint8_t		vs;		/* Voice specifier */
	uint8_t		vix;		/* Voice index */
	uint8_t		wc;		/* Word count */
	uint8_t		inhibit;	/* Input inhibit */
	int16_t		samples[FRAMES_PER_PERIOD * SND_CHANNELS];
	struct voice	voices[VOICES];
	uint32_t	lde_count;
#if NO_TERMINAL
	uint16_t	next_key;
	uint16_t	next_time;
#else
	uint32_t	key_bits;	/* Accumulated keyset bits */
	uint16_t	key_bit_count;	/* Count of bits accumulated */
#endif /* NO_TERMINAL */
	uint8_t		spi_buf[6];
};

/* setamp - Set amplitude on voice
 * @sess: Pointer to host_session
 * @vix: Index to voice to set
 * @ampix: GSW vaolume index (0 - 7)
 */
static void setamp(struct host_session *sess, int vix, int ampix)
{
	sess->voices[vix].amp = &amp[ampix];
}

/* setdiv() - Set divisor for voice
 * @sess: Pointer to host_session
 * @vix: Index to voice to set
 * @div: Divisor to set
 */
static void setdiv(struct host_session *sess, int vix, int div)
{
	struct voice *v = &sess->voices[vix];
	uint16_t step;

	v->div = div;
	step = (div + v->wave.nsamp - 1) / v->wave.nsamp;
	v->step = step;
	v->frac = frac_gen(step, &v->shift);
}

#define XOFF1LIMIT ((2 * HOST_IN_WORDS) / 3)
#define XOFF2LIMIT ((3 * HOST_IN_WORDS) / 4)
#define XON1LIMIT (HOST_IN_WORDS / 3)
#define XON2LIMIT (HOST_IN_WORDS / 4)

extern char *optarg;
extern int optind;
extern int optopt;
extern int opterr;
extern int optreset;

static int debug_flag;

static const char *port = "5004";	/* Default port number */
static const char *host = "cyberserv.org";
static const char *spi_dev = "/dev/spidev0.0";
static uint32_t	spi_speed = 4000;

static struct host_session sess = {
	.pending_echo = -1,
};

static snd_pcm_t *snd_ph;	/* Playback handle */
static snd_pcm_hw_params_t *snd_hw_params;
static snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
static const char pcm_name[] = "hw:0,0";

static snd_async_handler_t *pcm_handler;

struct fd_proc {
	void	(*poll)(void *data, int revents);
	void	*data;
};

static struct fd_proc	*fd_proc;
static struct pollfd	*fds;
static int nfds;

static void
register_fd(int fd, void (*func)(void *, int), int events, void *data)
{
	++nfds;
	if (!fd_proc) {
		fd_proc = malloc(nfds * sizeof(*fd_proc));
		fds = malloc(nfds * sizeof(*fds));
	} else {
		fd_proc = realloc(fd_proc, nfds * sizeof(*fd_proc));
		fds = realloc(fds, nfds * sizeof(*fds));
	}
	if (!fd_proc || !fds) {
		fprintf(stderr, "%s: malloc failure\n", __func__);
		exit(1);
	}
	fd_proc[nfds - 1].poll = func;
	fd_proc[nfds - 1].data = data;
	fds[nfds - 1].fd = fd;
	fds[nfds - 1].events = events;
	fds[nfds - 1].revents = 0;
}

static int do_poll(int timeout)
{
	int ix;
	int n;

	n = poll(fds, nfds, timeout);
	if (n < 0) {
		int err = errno;

		fprintf(stderr, "%s: poll error, errno=%d\n", __func__, err);
		errno = err;
		return -1;
	}
	if (n == 0)
		return 0;

	for (ix = 0; ix < nfds; ++ix) {
		if (fds[ix].revents) {
			fd_proc[ix].poll(fd_proc[ix].data, fds[ix].revents);
			fds[ix].revents = 0;
		}
	}
	return n;
}

static void gsw_callback(snd_async_handler_t *handler)
{
	fprintf(stderr, "%s: handler=%p\n", __func__, handler);
}

#if 0
static long timediff(const struct timespec *last, const struct timespec *now)
{
	int secs;
	long nsecs;

	secs = now->tv_sec - last->tv_sec;
	nsecs = now->tv_nsec - last->tv_nsec;
	if (nsecs < 0) {
		--secs;
		nsecs += 1000000000L;
	}
	nsecs += 1000000000 * secs;
	return nsecs;
}
#endif /* 0 */

/* host_word_count() - Return count of host words in buffer
 * @sess: Pointer to host_session
 *
 * Returns number of buffered words from host
 */
static unsigned int host_word_count(struct host_session *sess)
{
	int	diff = sess->inwd_in - sess->inwd_out;

	if (diff < 0)
		diff += ARRAY_SIZE(sess->inwds);
	if (diff < 0) {
		fprintf(stderr, "inwd_in/inwd_out inconsistency, in=%d, out=%d\n",
			sess->inwd_in, sess->inwd_out);
		sess->inwd_in = sess->inwd_out = diff = 0;
	}
	return diff;
}

#define KEY_NEXT	026
//#define KEY_STOP1	01640
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

/* send_key() - Send key code to host
 * @sess: PLATO host session
 * @key: PLATO key code to send
 */
static void send_key(struct host_session *sess, uint16_t key)
{
	uint8_t keybuf[2];
	int rc;

	keybuf[0] = key >> 7;
	keybuf[1] = 0200 | key;
	rc = send(sess->fd, keybuf, sizeof(keybuf), MSG_NOSIGNAL);
	if (rc != sizeof(keybuf)) {
		if (rc < 0)
			fprintf(stderr, "error on send - %m\n");
		else
			fprintf(stderr, "wrong size = %d\n", rc);
		return;
	}
	fprintf(stderr, "send %04o [%s]\n", key,
		key_decode[key] ? key_decode[key] : "");
}

/* echo_handle() - Check for echo commands and handle them
 * @sess: Pointer to host_session structure
 * @word: Word to check
 *
 * Return 0 if echo command, else return input
 */
static uint32_t echo_handle(struct host_session *sess, uint32_t word)
{
	uint32_t data;
	int16_t nwds;

	if (word & (1 << 19))		/* If a data word */
		return word;

	data = (word >> 1) & 0x7FFF;	/* Extract only the data */
	if (((word >> 16) & 7) != 3)	/* If not load echo command */
		return word;

	nwds = host_word_count(sess);
	if (nwds > XOFF1LIMIT) {
		sess->pending_echo = (data & 0x7F) | 0x80;
//		send_key(sess, KEY_XON);
	} else {
		send_key(sess, (data & 0x7F) | 0x80);
		sess->pending_echo = -1;
	}
	return 0;
}

/* gsw_handle() - Check for GSW commands and handle them
 * @sess: Pointer to host_session structure
 * @word: 21-bit PLATO output word
 *
 * Return NOP of GSW command, else return input
 */
static uint32_t gsw_handle(struct host_session *sess, uint32_t word)
{
	uint32_t data;

	if (word & (1 << 19))		/* If a data word */
		return word;		/* Always pass data words */

	data = (word >> 1) & 0x7FFF;	/* Extract only the data */
	switch ((word >> 16) & 7) {
	case CMD_AUD:		/* If audio command */
		if ((word & 0x7800)) {	/* If not GSW NOP */
			sess->cis = (data & 0x8000) != 0;
			sess->vix = sess->vs = (data >> 12) & 3;
			setamp(sess, 0, (data >> 9) & 7);
			setamp(sess, 1, (data >> 6) & 7);
			setamp(sess, 2, (data >> 3) & 7);
			setamp(sess, 3, data & 7);
		}
		break;

	case CMD_EXT:		/* If ext command */
		setdiv(sess, sess->vix, E2D(data & 0xFFFFF));
		if (!sess->cis) {
			if (sess->vix)
				--sess->vix;
			else
				sess->vix = sess->vs;
		}
		break;

	default:
		return word;		/* Return original word for all else */
	}

	sess->gsw_words[sess->gsw_cnt++] = word;
	if (sess->gsw_cnt >= ARRAY_SIZE(sess->gsw_words))
		sess->gsw_cnt = 0;

	return 04000003;		/* Send NOP to terminal */
}

#if HOST_DECODE1 || HOST_DECODE2
/* chmem - Return possible characters
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

#define NOP_MASK_SPEC	077000	/* Special data */
#define NOP_SETSTAT	042000	/* Set station number */
#define NOP_PMDSTART	043000	/* Start streaming "Plato Meta Data" */
#define NOP_PMDSTREAM	044000	/* Plato Meta Data stream */
#define NOP_PMDSTOP	045000	/* Stop Plato Meta Data stream */
#define NOP_FONTTYPE	050000	/* Font type */
#define NOP_FONTSIZE	051000	/* Font size */
#define NOP_FONTFLAG	052000	/* Font flags */
#define NOP_FONTINFO	053000	/* Get last font character width/height */
#define NOP_OSINFO	054000	/* Get OS type, 1=mac, 2=win, 3=linux */

/* decode_nop() - Decode special NOP
 *
 * Returns true if special NOP
 */
static bool decode_nop(uint32_t word)
{
	uint32_t w;

	word >>= 1;
	w = word & NOP_MASK_SPEC;
	switch (w) {
	case NOP_SETSTAT:
		word &= 0777;
		fprintf(stderr, "NOP: station=%d-%d\n", word >> 5, word & 31);
		return true;

	case NOP_FONTTYPE:
		fprintf(stderr, "NOP: font type=%02o\n", word & 077);
		return true;

	case NOP_FONTSIZE:
		fprintf(stderr, "NOP: font size=%02o\n", word & 077);
		return true;

	case NOP_FONTFLAG:
		fprintf(stderr, "NOP: font flag=%02o\n", word & 077);
		return true;

	case NOP_FONTINFO:
		fprintf(stderr, "NOP: font info\n");
		return true;

	case NOP_OSINFO:
		fprintf(stderr, "NOP: os info\n");
		return true;

	case NOP_PMDSTART:
		fprintf(stderr, "NOP: PMD start: %02o\n", word & 077);
		return true;

	case NOP_PMDSTREAM:
		fprintf(stderr, "NOP: PMD stream: %02o\n", word & 077);
		return true;

	case NOP_PMDSTOP:
		fprintf(stderr, "NOP: PMD stop: %02o\n", word & 077);
		return true;
	}
	return false;
}

/* decode_host_word - Decode host word
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
		if (!decode_nop(w))
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

static bool is_screen_clear(uint32_t w)
{
	enum terminal_cmd_codes cmd = (w >> 16) & 7;

	if (w & (1 << 19))	/* If not a command word */
		return false;
	if (cmd != CMD_LDM)	/* If not LDM command */
		return false;
	if (w & 2)
		return true;
	return false;
}

static bool is_abortable_command(struct host_session *sess, uint32_t w)
{
	enum terminal_cmd_codes cmd = (w >> 16) & 7;

	if (w & (1 << 19)) {	/* If not a command word */
		w >>= 1;
		if (sess->current_mode == 3 && (w & 0777700) == 0777700)
			return false;
		return sess->current_mode != 2;
	}

	switch (cmd) {
	case CMD_NOP:
	case CMD_SSL:
	case CMD_AUD:
	case CMD_EXT:
		return true;

	case CMD_LDM:
	case CMD_LDC:
	case CMD_LDE:
	case CMD_LDA:
		return false;
	}
	return true;
}

static void track_mode(struct host_session *sess, uint32_t w)
{
	enum terminal_cmd_codes cmd = (w >> 16) & 7;

	if (w & (1 << 19))	/* If not a command word */
		return;
	if (cmd != CMD_LDM)
		return;
	sess->current_mode = (w >> 4) & 3;
}

/* get_host_word() - Get next host word from buffer
 * @sess: Pointer to host_session
 *
 * Returns next unaborted word to send to terminal
 */
static uint32_t get_host_word(struct host_session *sess)
{
	uint32_t word;
	uint16_t tmp_out = sess->inwd_out;

	do {
		word = sess->inwds[tmp_out++];
		if (tmp_out >= ARRAY_SIZE(sess->inwds))
			tmp_out = 0;
		if (!sess->erase_abort_count)
			break;
		if (is_screen_clear(word))
			--sess->erase_abort_count;
		if (!is_abortable_command(sess, word))
			break;
		fprintf(stderr, "A\n");
		decode_host_word(word);
	} while (sess->erase_abort_count);
	sess->inwd_out = tmp_out;
	return word;
}

/* do_host_word() - Process any host word
 * @sess: Pointer to host_session
 *
 * Returns any word to send to attached terminal
 */
static uint32_t do_host_word(struct host_session *sess)
{
	uint32_t word;
	int16_t nwds;

	if (sess->inwd_in == sess->inwd_out)
		return 04000003;

	word = get_host_word(sess);
	sess->wc = (sess->wc + 1) & 0177;
#if HOST_DECODE2
	decode_host_word(word);
#endif /* HOST_DECODE2 */
	track_mode(sess, word);
	word = echo_handle(sess, word);
	if (!word)
		++sess->lde_count;
	nwds = host_word_count(sess);
	if (nwds < XOFF1LIMIT && sess->pending_echo != -1) {
		send_key(sess, sess->pending_echo);
		sess->pending_echo = -1;
	}
	if (nwds == XON1LIMIT || nwds == XON2LIMIT) {
		fprintf(stderr, "nwds=%d ", nwds);
		send_key(sess, KEY_XON);
	}
	if (!word)
		return 0;
#if NO_TERMINAL
	if ((word & 07640000) == 04240000)
		sess->wc = (word >> 7) & 0177;
	if ((word & 07600000) == 04200000)
		sess->inhibit = !!(word & 00100000);
#endif /* NO_TERMINAL */
	word = gsw_handle(sess, word);
	return word;
}

#if NO_TERMINAL
struct keys {
	uint16_t delay;		/* Delay in 1/60th second intervals */
	uint16_t key;		/* Key to send */
};

const struct keys keys[] = {
	{ 5, KEY_TURNON },
	{ 600, KEY_NEXT },
	{ 600, LC_KEY('r') },
	{ 20, LC_KEY('u') },
	{ 20, LC_KEY('s') },
	{ 20, LC_KEY('t') },
	{ 20, LC_KEY('a') },
	{ 20, LC_KEY('d') },
	{ 60, KEY_NEXT },
	{ 600, LC_KEY('c') },
	{ 20, LC_KEY('f') },
	{ 20, LC_KEY('r') },
	{ 20, LC_KEY('e') },
	{ 20, LC_KEY('a') },
	{ 20, LC_KEY('k') },
	{ 20, LC_KEY('s') },
	{ 60, KEY_STOP1 },
	{ 600, LC_KEY('g') },
	{ 30, LC_KEY('o') },
	{ 30, LC_KEY('o') },
	{ 30, LC_KEY('c') },
	{ 30, LC_KEY('h') },
	{ 80, KEY_NEXT },
	{ 600, LC_KEY('g') },
	{ 20, LC_KEY('s') },
	{ 20, LC_KEY('w') },
	{ 20, LC_KEY('a') },
	{ 20, LC_KEY('i') },
	{ 20, LC_KEY('d') },
	{ 20, LC_KEY('s') },
	{ 60, KEY_DATA },
	{ 600, LC_KEY('d') },
	{ 600, LC_KEY('b') },
};

const uint16_t num_keys = ARRAY_SIZE(keys);
#endif /* NO_TERMINAL */

/* send_word() - Send word to terminal
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
		.speed_hz = 4000,
		.bits_per_word = 8,
	};
	unsigned int i;
	int rc;

	word <<= 11;
	bytes[0] = word >> 24;
	bytes[1] = word >> 16;
	bytes[2] = word >> 8;
	for (i = 3; i < sizeof(bytes); ++i)
		bytes[i] = 0;
	rc = ioctl(sess->spi_fd, SPI_IOC_MESSAGE(1), &spi_xfer);
	if (rc < 0) {
		fprintf(stderr, "%s: write error: %m\n", __func__);
		return;
	}
}

#if !NO_TERMINAL
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

static void abort_all_output(struct host_session *sess)
{
	sess->inwd_out = sess->inwd_in;
	sess->erase_abort_count = 0;
}

static void process_spi_input(struct host_session *sess)
{
	unsigned int i;
	uint8_t	*bytes = sess->spi_buf;

	for (i = 0; i < sizeof(sess->spi_buf); ++i) {
		if (sess->key_bit_count == 0) {
			if (bytes[i] == 0)
				continue;
			sess->key_bit_count = fls(bytes[i]);
			sess->key_bits = bytes[i];
			continue;
		}
		sess->key_bits = (sess->key_bits << 8) | bytes[i];
		sess->key_bit_count += 8;
		if (sess->key_bit_count >= 12) {
			uint16_t key_data;
			int bits_remaining = sess->key_bit_count - 12;

			key_data = sess->key_bits >> bits_remaining;
			key_data = (key_data >> 1) & 0x3ff;
			fprintf(stderr, "Send keyset data = %4o\n", key_data);
			send_key(sess, key_data);
			if (key_data == KEY_STOP || key_data == KEY_STOP1)
				abort_all_output(sess);
			sess->key_bits &= (1 << bits_remaining) - 1;
			sess->key_bit_count -= 12;
			if (sess->key_bits == 0)
				sess->key_bit_count = 0;
			else
				sess->key_bit_count = fls(sess->key_bits);
		}
	}

	return;
}
#endif /* ! NO_TERMINAL */

static void gsw_poll(void *p, int event)
{
	int rc;
	struct timespec ts;
//	static struct timespec last;
	struct host_session *sess = p;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		fprintf(stderr, "clock_gettime failed with %d, %m\n", errno);
#if 0
	else
		fprintf(stderr, "diff %ld, event=%x\n",
			timediff(&last, &ts) / 1000L, event);
	last = ts;
#endif /* 0 */

	if (event & POLLERR) {
		fprintf(stderr, "%s: error set\n", __func__);
		snd_pcm_prepare(snd_ph);
	}

	if (event & POLLOUT) {
		int i;
		int voice;
		uint32_t out_word;

		rc = snd_pcm_writei(snd_ph, sess->samples,
				    sizeof(sess->samples) / FRAME_SIZE);
		if (rc < 0) {
			fprintf(stderr, "%s: error on snd write, rc=%d\n",
				__func__, rc);
			return;
		}
		out_word = do_host_word(sess);
		send_word(sess, out_word);
		for (i = 0; i < (int)ARRAY_SIZE(sess->samples); ++i) {
			uint32_t sample = 0;
			struct voice *v = &sess->voices[0];

			for (voice = 0; voice < VOICES; ++voice, ++v)
				sample += generate(v);

			sample >>= NVSHIFT;
			sess->samples[i] = sample;
			sess->samples[++i] = sample;
		}
#if NO_TERMINAL
		if (/*sess->lde_count >= LDE_WAIT &&*/ sess->next_key < num_keys &&
		    --sess->next_time == 0) {
			send_key(sess, keys[sess->next_key].key);
			++sess->next_key;
			if (sess->next_key < num_keys)
				sess->next_time = keys[sess->next_key].delay;
			else
				fprintf(stderr, "done sending keys\n");
		}
#else
		process_spi_input(sess);
#endif /* NO_TERMINAL */
	}
}

static int open_gsw(struct host_session *sess)
{
	int i;
	int err;

	err = snd_pcm_hw_params_malloc(&snd_hw_params);
	if (err < 0) {
		fprintf(stderr, "Error allocating hw_params structure\n");
		return -1;
	}

	err = snd_pcm_open(&snd_ph, pcm_name, stream,
			   SND_PCM_NONBLOCK | SND_PCM_ASYNC);
	if (err < 0) {
		fprintf(stderr, "Error opening PCM device %s\n", pcm_name);
		return -1;
	}

	err = snd_pcm_hw_params_any(snd_ph, snd_hw_params);
	if (err < 0) {
		fprintf(stderr, "Error setting up hw_params structure\n");
		return -1;
	}

	err = snd_pcm_hw_params_set_access(snd_ph, snd_hw_params,
					   SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0) {
		fprintf(stderr, "Error setting access\n");
		return -1;
	}

	err = snd_pcm_hw_params_set_format(snd_ph, snd_hw_params,
					   SND_PCM_FORMAT_S16);
	if (err < 0) {
		fprintf(stderr, "Error setting format\n");
		return -1;
	}

	unsigned int exact = SND_RATE;

	err = snd_pcm_hw_params_set_rate_near(snd_ph, snd_hw_params, &exact, 0);
	if (err < 0) {
		fprintf(stderr, "Error setting rate\n");
		return -1;
	}
	fprintf(stderr, "Exact rate = %d, SND_RATE = %d\n", exact, SND_RATE);

	err = snd_pcm_hw_params_set_channels(snd_ph, snd_hw_params,
					     SND_CHANNELS);
	if (err < 0) {
		fprintf(stderr, "Error setting channels\n");
		return -1;
	}

	err = snd_pcm_hw_params_set_periods(snd_ph, snd_hw_params,
					    SND_PERIODS, 0);
	if (err < 0) {
		fprintf(stderr, "Error setting periods\n");
		return -1;
	}

	snd_pcm_uframes_t min;
	snd_pcm_uframes_t max;

	err = snd_pcm_hw_params_get_buffer_size_min(snd_hw_params, &min);
	if (err < 0) {
		fprintf(stderr, "Error getting min\n");
		return -1;
	}
	err = snd_pcm_hw_params_get_buffer_size_max(snd_hw_params, &max);
	if (err < 0) {
		fprintf(stderr, "Error getting max\n");
		return -1;
	}
	fprintf(stderr, "size=%d, min=%lu, max=%lu\n",
		SND_BUFFER_SIZE, min, max);

	err = snd_pcm_hw_params_set_buffer_size(snd_ph, snd_hw_params,
						SND_BUFFER_SIZE);
	if (err < 0) {
		fprintf(stderr, "Error setting buffer size, err=%d, errno=%d\n",
			err, errno);
		return -1;
	}

	err = snd_pcm_hw_params(snd_ph, snd_hw_params);
	if (err < 0) {
		fprintf(stderr, "Error setting hw params\n");
		return -1;
	}

	int cnt = snd_pcm_poll_descriptors_count(snd_ph);
	if (cnt != 1) {
		fprintf(stderr, "Bad descriptor count = %d\n", cnt);
		return -1;
	}

	struct pollfd fds;

	cnt = snd_pcm_poll_descriptors(snd_ph, &fds, 1);
	if (cnt != 1) {
		fprintf(stderr, "Returned descriptor error = %d\n", cnt);
		return -1;
	}

	register_fd(fds.fd, gsw_poll, POLLOUT | POLLERR, sess);

	err = snd_async_add_pcm_handler(&pcm_handler, snd_ph, gsw_callback,
					NULL);
	if (err < 0) {
		fprintf(stderr, "Failed to add handler, err=%d\n", err);
		return -1;
	}

	for (i = 0; i < SND_PERIODS; ++i) {
		err = snd_pcm_writei(snd_ph, silence,
				     sizeof(silence) / FRAME_SIZE);
		if (err < 0) {
			fprintf(stderr, "%s: error on snd write\n", __func__);
			return -1;
		}
	}

	return 0;
}

/* open_host - Open a session to the host
 * @host: Pointer to host DNS name
 *
 * Returns file descriptor or -1 if error
 */
static int open_host(const char *host)
{
	int	s;
	int	rc;
	int	err;
	const char	*cause = NULL;
	struct addrinfo	hints;
	struct addrinfo *res, *res0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo(host, port, &hints, &res0);
	if (rc < 0) {
		fprintf(stderr, "Failed to get address of host %s\n", host);
		return -1;
	}
	s = -1;
	err = 0;
	for (res = res0; res; res = res->ai_next) {
		int true_opt = 1;

		s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (s < 0) {
			cause = "socket";
			err = errno;
			continue;
		}
		if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
			cause = "socket";
			err = errno;
			close(s);
			s = -1;
			continue;
		}
		setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char *)&true_opt,
			   sizeof(true_opt));
		fcntl(s, F_SETFL, O_NONBLOCK);
		break;
	}
	freeaddrinfo(res0);
	if (s < 0) {
		fprintf(stderr, "%s() failed for host %s\n", cause, host);
		errno = err;
		return -1;
	}
	return s;
}

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

/* host_word - Accumulate host word
 * @sess: Pointer to host_session
 * @buf: Pointer to 3-byte input buffer
 *
 * Returns accumulated word
 */
static int32_t host_word(struct host_session *sess, uint8_t *buf)
{
	uint32_t	w;

	if ((buf[0] & 0200) || (buf[1] & 0300) != 0200 ||
	    (buf[2] & 0300) != 0300) {
		sess->host_state = out_of_sync;
		return -1;
	}
	w = (buf[0] << 12) | ((buf[1] & 077) << 6) | (buf[2] & 077);
	w <<= 1;
	w |= (1 << 20) | host_word_parity(w);
	return w;
}

/* put_host_word - Put host word into buffer
 * @sess: Pointer to host_session structure
 * @w: Host word
 */
static void put_host_word(struct host_session *sess, uint32_t w)
{
	uint16_t tmp_ix;

	tmp_ix = sess->inwd_in;
	if (is_screen_clear(w))
		++sess->erase_abort_count;
	sess->inwds[tmp_ix++] = w;
	if (tmp_ix == ARRAY_SIZE(sess->inwds))
		tmp_ix = 0;
	if (tmp_ix == sess->inwd_out) {
		fprintf(stderr, "host word overflow\n");
		return;
	}
	sess->inwd_in = tmp_ix;
}

/* usage - Print command usage information
 */
static void usage(const char *cmd)
{
	fprintf(stderr, "%s: Command usage:\n", cmd);
	fprintf(stderr,
		"\t-d\tEnable debugging\n"
		"\t-h\tDisplay this help\n"
		"\t-p\tPort number (default 5004)\n"
		"\t-r\tSPI rate\n"
		"\t-s\tSPI device path\n");
}

/* process_arguments - Process arguments
 * @argc: Number of arguments
 * @argv: Pointer to an array of pointers to arguments
 *
 * Return 0 if success, non-zero on some error
 */
static int process_arguments(int argc, char *argv[])
{
	int ch;
	const char *cmd = argv[0];

	while ((ch = getopt(argc, argv, "dhp:r:s:")) != -1) {
		switch (ch) {
		case 'd':
			++debug_flag;
			break;
		case 'h':
			usage(cmd);
			exit(0);
		case 'p':
			port = optarg;
			break;
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

	if (argc == 1)
		host = argv[optind];
	else if (argc > 1) {
		fprintf(stderr, "Too many arguments\n");
		return 4;
	}

	return 0;
}

static void host_poll(void *data, int revents)
{
	struct host_session *sess = data;
	uint8_t	inbuf[3];
	unsigned int i;

	if (revents & POLLERR) {
		fprintf(stderr, "Error set\n");
		return;
	}
	if (revents & POLLIN) {
		ssize_t len;

		if (sess->host_state == out_of_sync) {
			len = recv(sess->fd, &inbuf[0], 1, MSG_NOSIGNAL);
			if (len != 1)
				return;
			if (inbuf[0] & 0200) {
				fprintf(stderr, "0200 set - oos\n");
				return;
			}
			for (i = 1; i < sizeof(inbuf); ++i) {
				len = recv(sess->fd, &inbuf[i], 1,
					   MSG_NOSIGNAL);
				if (len != 1) {
					fprintf(stderr, "len wrong %zd\n", len);
					return;
				}
			}
			len = sizeof(inbuf);
			sess->host_state = in_sync;
		} else {
			len = recv(sess->fd, inbuf, sizeof(inbuf),
				   MSG_NOSIGNAL);
			if (len != sizeof(inbuf)) {
				fprintf(stderr, "len wrong %zd\n", len);
				sess->host_state = out_of_sync;
				return;
			}
		}
		if (len != sizeof(inbuf)) {
			if (len < 0) {
				int err = errno;

				fprintf(stderr, "Error on recv, err=%d\n", err);
				errno = err;
				return;
			}
			if (len == 0) {
				fprintf(stderr, "No data\n");
				return;
			}
			fprintf(stderr, "Wrong size=%zd\n", len);
			return;
		}

		int32_t	w = host_word(sess, inbuf);
		uint32_t count;

		if (w < 0) {
			fprintf(stderr, "w = %04x\n", w);
			return;
		}

#if HOST_DECODE1
		decode_host_word(w);
#endif /* HOST_DECODE1 */
		put_host_word(sess, w);
		count = host_word_count(sess);
		if (count == XOFF1LIMIT || count == XOFF2LIMIT) {
			fprintf(stderr, "count=%d ", count);
			send_key(sess, KEY_XOFF);
		}
	} else {
		fprintf(stderr, "revents=%04x\n", revents);
	}
}

/* open_spi() - Open spi device
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

	mode = SPI_NO_CS | SPI_MODE_1;
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
	fprintf(stderr, "mode=%d\n", mode);

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
	fprintf(stderr, "bits=%d\n", bits);

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
	fprintf(stderr, "speed=%d\n", speed);

	return fd;

err_exit:
	close(fd);
	return -1;
}

/* main() - Main program
 * @argc: Count of arguments passed
 * @argv: Pointer to an array of pointers to arguments
 *
 * Returns exit status
 */
int main(int argc, char *argv[])
{
	int rc;
	int i;

	rc = process_arguments(argc, argv);
	if (rc) {
		usage(argv[0]);
		return rc;
	}

	for (i = 0; i < VOICES; ++i)
		sess.voices[i].wave = sq;

#if NO_TERMINAL
	sess.next_time = keys[0].delay;
#endif /* NO_TERMINAL */

	sess.spi_fd = open_spi(spi_dev, spi_speed);
	if (sess.spi_fd < 0) {
		int err = errno;

		fprintf(stderr, "Failed to open SPI device %s, errno=%d\n",
			spi_dev, err);
		exit(err);
	}
	fcntl(sess.spi_fd, F_SETFL, O_NONBLOCK);

	sess.fd = open_host(host);
	if (sess.fd < 0) {
		int err = errno;

		fprintf(stderr, "Failed to open host %s, errno=%d\n", host, err);
		exit(err);
	}

	register_fd(sess.fd, host_poll, POLLERR | POLLIN, &sess);

	if (open_gsw(&sess) < 0) {
		fprintf(stderr, "open_gsw failed\n");
		return 1;
	}


	for (;;) {
		do_poll(5);
	}

	return 0;
}
