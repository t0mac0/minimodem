/*
 * minimodem.c
 *
 * minimodem - software audio Bell-type or RTTY FSK modem
 *
 * Copyright (C) 2011-2012 Kamal Mostafa <kamal@whence.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>
#include <signal.h>
#include <sys/time.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define VERSION "unknown"
#endif

#include "simpleaudio.h"
#include "fsk.h"
#include "baudot.h"

char *program_name = "";


/*
 * ASCII 8-bit data framebits decoder/encoder (passthrough)
 */

/* returns the number of datawords stuffed into *databits_outp */
int
framebits_encode_ascii8( unsigned int *databits_outp, char char_out )
{
    *databits_outp = char_out;
    return 1;
}

/* returns nbytes decoded */
static unsigned int
framebits_decode_ascii8( char *dataout_p, unsigned int dataout_size,
	unsigned int bits )
{
    if ( dataout_p == NULL )	// frame processor reset: noop
	return 0;
    assert( (bits & ~0xFF) == 0 );
    assert( dataout_size >= 1);
    *dataout_p = bits;
    return 1;
}


/*
 * Baudot 5-bit data framebits decoder/encoder
 */

#define framebits_encode_baudot baudot_encode

/* returns nbytes decoded */
static unsigned int
framebits_decode_baudot( char *dataout_p, unsigned int dataout_size,
	unsigned int bits )
{
    if ( dataout_p == NULL ) {	// frame processor reset: reset Baudot state
	    baudot_reset();
	    return 0;
    }
    assert( (bits & ~0x1F) == 0 );
    assert( dataout_size >= 1);
    return baudot_decode(dataout_p, bits);
}



int		tx_transmitting = 0;
int		tx_leader_bits_len = 2;
int		tx_trailer_bits_len = 2;

simpleaudio	*tx_sa_out;
float		tx_bfsk_mark_f;
unsigned int	tx_bit_nsamples;

void
tx_stop_transmit_sighandler( int sig )
{
    // fprintf(stderr, "alarm\n");

    int j;
    for ( j=0; j<tx_trailer_bits_len; j++ )
	simpleaudio_tone(tx_sa_out, tx_bfsk_mark_f, tx_bit_nsamples);

    // 0.5 sec of zero samples to flush - FIXME lame
    size_t sample_rate = simpleaudio_get_rate(tx_sa_out);
    simpleaudio_tone(tx_sa_out, 0, sample_rate/2);

    tx_transmitting = 0;
}


/*
 * rudimentary BFSK transmitter
 */
static void fsk_transmit_stdin(
	simpleaudio *sa_out,
	int tx_interactive,
	float data_rate,
	float bfsk_mark_f,
	float bfsk_space_f,
	int n_data_bits,
	float bfsk_txstopbits,
	int (*framebits_encoder)( unsigned int *databits_outp, char char_out )
	)
{
    size_t sample_rate = simpleaudio_get_rate(sa_out);
    size_t bit_nsamples = sample_rate / data_rate + 0.5;
    int c;

    tx_sa_out = sa_out;
    tx_bfsk_mark_f = bfsk_mark_f;
    tx_bit_nsamples = bit_nsamples;

    // one-shot
    struct itimerval itv = {
	{0, 0},						// it_interval
	{0, 1000000/(float)(data_rate+data_rate*0.03) }	// it_value
    };

    if ( tx_interactive )
	signal(SIGALRM, tx_stop_transmit_sighandler);

    tx_transmitting = 0;
    while ( (c = getchar()) != EOF )
    {
	if ( tx_interactive )
	    setitimer(ITIMER_REAL, NULL, NULL);

	// fprintf(stderr, "<c=%d>", c);
	unsigned int nwords;
	unsigned int bits[2];
	nwords = framebits_encoder(bits, c);

	if ( !tx_transmitting )
	{
	    tx_transmitting = 1;
	    int j;
	    for ( j=0; j<tx_leader_bits_len; j++ )
		simpleaudio_tone(sa_out, bfsk_mark_f, bit_nsamples);
	}
	unsigned int j;
	for ( j=0; j<nwords; j++ ) {
	    simpleaudio_tone(sa_out, bfsk_space_f, bit_nsamples);	// start
	    int i;
	    for ( i=0; i<n_data_bits; i++ ) {				// data
		unsigned int bit = ( bits[j] >> i ) & 1;
		float tone_freq = bit == 1 ? bfsk_mark_f : bfsk_space_f;
		simpleaudio_tone(sa_out, tone_freq, bit_nsamples);
	    }
	    simpleaudio_tone(sa_out, bfsk_mark_f,
				bit_nsamples * bfsk_txstopbits);	// stop
	}

	if ( tx_interactive )
	    setitimer(ITIMER_REAL, &itv, NULL);
    }
    if ( tx_interactive ) {
	setitimer(ITIMER_REAL, NULL, NULL);
	signal(SIGALRM, SIG_DFL);
    }
    if ( !tx_transmitting )
	return;

    tx_stop_transmit_sighandler(0);
}


static void
report_no_carrier( fsk_plan *fskp,
	unsigned int sample_rate,
	float bfsk_data_rate,
	float nsamples_per_bit,
	unsigned int nframes_decoded,
	size_t carrier_nsamples,
	float confidence_total )
{
    unsigned long long nbits_total = nframes_decoded * (fskp->n_data_bits+2);
#if 0
    fprintf(stderr, "nframes_decoded=%u\n", nframes_decoded);
    fprintf(stderr, "nbits_total=%llu\n", nbits_total);
    fprintf(stderr, "carrier_nsamples=%lu\n", carrier_nsamples);
    fprintf(stderr, "nsamples_per_bit=%f\n", nsamples_per_bit);
#endif
    float throughput_rate = nbits_total * sample_rate / (float)carrier_nsamples;
    fprintf(stderr, "### NOCARRIER ndata=%u confidence=%.3f throughput=%.2f",
	    nframes_decoded,
	    confidence_total / nframes_decoded,
	    throughput_rate);
    if ( (size_t)(nbits_total * nsamples_per_bit + 0.5) == carrier_nsamples ) {
	fprintf(stderr, " (rate perfect) ###\n");
    } else {
	float throughput_skew = (throughput_rate - bfsk_data_rate)
			    / bfsk_data_rate;
	fprintf(stderr, " (%.1f%% %s) ###\n",
		fabs(throughput_skew) * 100.0,
		signbit(throughput_skew) ? "slow" : "fast"
		);
    }
}

void
generate_test_tones( simpleaudio *sa_out, unsigned int duration_sec )
{
    unsigned int sample_rate = simpleaudio_get_rate(sa_out);
    unsigned int nframes = sample_rate / 10;
    int i;
    for ( i=0; i<(sample_rate/nframes*duration_sec); i++ ) {
	simpleaudio_tone(sa_out, 1000, nframes/2);
	simpleaudio_tone(sa_out, 1777, nframes/2);
    }
}

static int
benchmarks()
{
    fprintf(stdout, "minimodem %s benchmarks\n", VERSION);

    int ret;
    ret = system("sed -n -e '/^model name/{p;q}' -e '/^cpu model/{p;q}' /proc/cpuinfo");
    ret = ret;	// don't care, hush compiler.

    fflush(stdout);

    unsigned int sample_rate = 48000;
    sa_backend_t backend = SA_BACKEND_BENCHMARK;
    // backend = SA_BACKEND_SYSDEFAULT;	// for test

    simpleaudio *sa_out;


    // enable the sine wave LUT
    simpleaudio_tone_init(1024);

    sa_out = simpleaudio_open_stream(backend, SA_STREAM_PLAYBACK,
			SA_SAMPLE_FORMAT_S16, sample_rate, 1,
			program_name, "generate-tones-lut1024-S16-mono");
    if ( ! sa_out )
	return 0;
    generate_test_tones(sa_out, 10);
    simpleaudio_close(sa_out);

    sa_out = simpleaudio_open_stream(backend, SA_STREAM_PLAYBACK,
			SA_SAMPLE_FORMAT_FLOAT, sample_rate, 1,
			program_name, "generate-tones-lut1024-FLOAT-mono");
    if ( ! sa_out )
	return 0;
    generate_test_tones(sa_out, 10);
    simpleaudio_close(sa_out);


    // disable the sine wave LUT
    simpleaudio_tone_init(0);

    sa_out = simpleaudio_open_stream(backend, SA_STREAM_PLAYBACK,
			SA_SAMPLE_FORMAT_S16, sample_rate, 1,
			program_name, "generate-tones-nolut-S16-mono");
    if ( ! sa_out )
	return 0;
    generate_test_tones(sa_out, 10);
    simpleaudio_close(sa_out);

    sa_out = simpleaudio_open_stream(backend, SA_STREAM_PLAYBACK,
			SA_SAMPLE_FORMAT_FLOAT, sample_rate, 1,
			program_name, "generate-tones-nolut-FLOAT-mono");
    if ( ! sa_out )
	return 0;
    generate_test_tones(sa_out, 10);
    simpleaudio_close(sa_out);


    return 1;
}


void
version()
{
    printf(
    "minimodem %s\n"
    "Copyright (C) 2011-2012 Kamal Mostafa <kamal@whence.com>\n"
    "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\n"
    "This is free software: you are free to change and redistribute it.\n"
    "There is NO WARRANTY, to the extent permitted by law.\n\n"
    "Written by Kamal Mostafa <kamal@whence.com>.\n",
	VERSION);
}

void
usage()
{
    fprintf(stderr,
    "usage: minimodem [--tx|--rx] [options] {baudmode}\n"
    "		    -t, --tx, --transmit, --write\n"
    "		    -r, --rx, --receive,  --read     (default)\n"
    "		[options]\n"
    "		    -a, --auto-carrier\n"
    "		    -c, --confidence {min-snr-threshold}\n"
    "		    -l, --limit {max-snr-search-limit}\n"
    "		    -8, --ascii		ASCII  8-N-1\n"
    "		    -5, --baudot	Baudot 5-N-1\n"
    "		    -f, --file {filename.flac}\n"
    "		    -b, --bandwidth {rx_bandwidth}\n"
    "		    -M, --mark {mark_freq}\n"
    "		    -S, --space {space_freq}\n"
    "		    -T, --txstopbits {m.n}\n"
    "		    -q, --quiet\n"
    "		    -R, --samplerate {rate}\n"
    "		    -V, --version\n"
    "		    -A, --alsa\n"
    "		    --lut={tx_sin_table_len}\n"
    "		    --float-samples\n"
    "		    --benchmarks\n"
    "		{baudmode}\n"
    "		    1200       Bell202  1200 bps --ascii\n"
    "		     300       Bell103   300 bps --ascii\n"
    "	    any_number_N       Bell103     N bps --ascii\n"
    "		    rtty       RTTY    45.45 bps --baudot\n"
    );
    exit(1);
}

int
main( int argc, char*argv[] )
{
    char *modem_mode = NULL;
    int TX_mode = -1;
    int quiet_mode = 0;
    float band_width = 0;
    unsigned int bfsk_mark_f = 0;
    unsigned int bfsk_space_f = 0;
    float bfsk_txstopbits = 0;
    unsigned int bfsk_n_data_bits = 0;
    int autodetect_shift;
    char *filename = NULL;

    float	carrier_autodetect_threshold = 0.0;

    // fsk_confidence_threshold : signal-to-noise squelch control
    //
    // The minimum SNR confidence level seen as "a signal".
    float fsk_confidence_threshold = 2.0;

    // fsk_confidence_search_limit : performance vs. quality
    //
    // If we find a frame with SNR confidence > confidence_search_limit,
    // quit searching for a better frame.  confidence_search_limit has a
    // dramatic effect on peformance (high value yields low performance, but
    // higher decode quality, for noisy or hard-to-discern signals (Bell 103).
    float fsk_confidence_search_limit = 2.3f;
    // float fsk_confidence_search_limit = INFINITY;  /* for test */

    sa_backend_t sa_backend = SA_BACKEND_SYSDEFAULT;
    sa_format_t sample_format = SA_SAMPLE_FORMAT_S16;
    unsigned int sample_rate = 48000;
    unsigned int nchannels = 1; // FIXME: only works with one channel

    unsigned int tx_sin_table_len = 4096;

    /* validate the default system audio mechanism */
#if !(USE_PULSEAUDIO || USE_ALSA)
# define _MINIMODEM_NO_SYSTEM_AUDIO
# if !USE_SNDFILE
#  error At least one of {USE_PULSEAUDIO,USE_ALSA,USE_SNDFILE} must be enabled!
# endif
#endif

    program_name = strrchr(argv[0], '/');
    if ( program_name )
	program_name++;
    else
	program_name = argv[0];

    int c;
    int option_index;
    
    enum {
	MINIMODEM_OPT_UNUSED=256,	// placeholder
	MINIMODEM_OPT_LUT,
	MINIMODEM_OPT_FLOAT_SAMPLES,
	MINIMODEM_OPT_BENCHMARKS,
    };

    while ( 1 ) {
	static struct option long_options[] = {
	    { "version",	0, 0, 'V' },
	    { "tx",		0, 0, 't' },
	    { "transmit",	0, 0, 't' },
	    { "write",		0, 0, 't' },
	    { "rx",		0, 0, 'r' },
	    { "receive",	0, 0, 'r' },
	    { "read",		0, 0, 'r' },
	    { "confidence",	1, 0, 'c' },
	    { "limit",		1, 0, 'l' },
	    { "auto-carrier",	0, 0, 'a' },
	    { "ascii",		0, 0, '8' },
	    { "baudot",		0, 0, '5' },
	    { "file",		1, 0, 'f' },
	    { "bandwidth",	1, 0, 'b' },
	    { "mark",		1, 0, 'M' },
	    { "space",		1, 0, 'S' },
	    { "txstopbits",	1, 0, 'T' },
	    { "quiet",		0, 0, 'q' },
	    { "alsa",		0, 0, 'A' },
	    { "samplerate",	1, 0, 'R' },
	    { "lut",		1, 0, MINIMODEM_OPT_LUT },
	    { "float-samples",	0, 0, MINIMODEM_OPT_FLOAT_SAMPLES },
	    { "benchmarks",	0, 0, MINIMODEM_OPT_BENCHMARKS },
	    { 0 }
	};
	c = getopt_long(argc, argv, "Vtrc:a85f:b:M:S:T:qAR:",
		long_options, &option_index);
	if ( c == -1 )
	    break;
	switch( c ) {
	    case 'V':
			version();
			exit(0);
	    case 't':
			if ( TX_mode == 0 )
			    usage();
			TX_mode = 1;
			break;
	    case 'r':
			if ( TX_mode == 1 )
			    usage();
			TX_mode = 0;
			break;
	    case 'c':
			fsk_confidence_threshold = atof(optarg);
			break;
	    case 'l':
			fsk_confidence_search_limit = atof(optarg);
			break;
	    case 'a':
			carrier_autodetect_threshold = 0.001;
			break;
	    case 'f':
			filename = optarg;
			break;
	    case '8':
			bfsk_n_data_bits = 8;
			break;
	    case '5':
			bfsk_n_data_bits = 5;
			break;
	    case 'b':
			band_width = atof(optarg);
			assert( band_width != 0 );
			break;
	    case 'M':
			bfsk_mark_f = atoi(optarg);
			assert( bfsk_mark_f > 0 );
			break;
	    case 'S':
			bfsk_space_f = atoi(optarg);
			assert( bfsk_space_f > 0 );
			break;
	    case 'T':
			bfsk_txstopbits = atof(optarg);
			assert( bfsk_txstopbits > 0 );
			break;
	    case 'q':
			quiet_mode = 1;
			break;
	    case 'R':
			sample_rate = atoi(optarg);
			assert( sample_rate > 0 );
			break;
	    case 'A':
#if USE_ALSA
			sa_backend = SA_BACKEND_ALSA;
#else
			fprintf(stderr, "E: This build of minimodem was configured without alsa support.\n");
			exit(1);
#endif
			break;
	    case MINIMODEM_OPT_LUT:
			tx_sin_table_len = atoi(optarg);
			break;
	    case MINIMODEM_OPT_FLOAT_SAMPLES:
			sample_format = SA_SAMPLE_FORMAT_FLOAT;
			break;
	    case MINIMODEM_OPT_BENCHMARKS:
			benchmarks();
			exit(0);
			break;
	    default:
			usage();
	}
    }
    if ( TX_mode == -1 )
	TX_mode = 0;

    /* The receive code requires floating point samples to feed to the FFT */
    if ( TX_mode == 0 )
	sample_format = SA_SAMPLE_FORMAT_FLOAT;

    if ( filename ) {
#if !USE_SNDFILE
	fprintf(stderr, "E: This build of minimodem was configured without sndfile,\nE:   so the --file flag is not supported.\n");
	exit(1);
#endif
    } else {
#ifdef _MINIMODEM_NO_SYSTEM_AUDIO
	fprintf(stderr, "E: this build of minimodem was configured without system audio support,\nE:   so only the --file mode is supported.\n");
	exit(1);
#endif
    }

#if 0
    if (optind < argc) {
	printf("non-option ARGV-elements: ");
	while (optind < argc)
	    printf("%s ", argv[optind++]);
	printf("\n");
    }
#endif

    if (optind + 1 !=  argc) {
	fprintf(stderr, "E: *** Must specify {baudmode} (try \"300\") ***\n");
	usage();
    }

    modem_mode = argv[optind++];


    float	bfsk_data_rate = 0.0;
    int (*bfsk_framebits_encode)( unsigned int *databits_outp, char char_out );

    unsigned int (*bfsk_framebits_decode)( char *dataout_p, unsigned int dataout_size,
					unsigned int bits );

    if ( strncasecmp(modem_mode, "rtty",5)==0 ) {
	bfsk_data_rate = 45.45;
	if ( bfsk_n_data_bits == 0 )
	    bfsk_n_data_bits = 5;
	if ( bfsk_txstopbits == 0 )
	    bfsk_txstopbits = 1.5;
    } else {
	bfsk_data_rate = atof(modem_mode);
	if ( bfsk_n_data_bits == 0 )
	    bfsk_n_data_bits = 8;
    }
    if ( bfsk_data_rate == 0.0 )
	usage();

    if ( bfsk_n_data_bits == 8 ) {
	bfsk_framebits_decode = framebits_decode_ascii8;
	bfsk_framebits_encode = framebits_encode_ascii8;
    } else if ( bfsk_n_data_bits == 5 ) {
	bfsk_framebits_decode = framebits_decode_baudot;
	bfsk_framebits_encode = framebits_encode_baudot;
    } else {
	assert( 0 && bfsk_n_data_bits );
    }

    if ( bfsk_data_rate >= 400 ) {
	/*
	 * Bell 202:     baud=1200 mark=1200 space=2200
	 */
	autodetect_shift = - ( bfsk_data_rate * 5 / 6 );
	if ( bfsk_mark_f == 0 )
	    bfsk_mark_f  = bfsk_data_rate / 2 + 600;
	if ( bfsk_space_f == 0 )
	    bfsk_space_f = bfsk_mark_f - autodetect_shift;
	if ( band_width == 0 )
	    band_width = 200;
    } else if ( bfsk_data_rate >= 100 ) {
	/*
	 * Bell 103:     baud=300 mark=1270 space=1070
	 * ITU-T V.21:   baud=300 mark=1280 space=1080
	 */
	autodetect_shift = 200;
	if ( bfsk_mark_f == 0 )
	    bfsk_mark_f  = 1270;
	if ( bfsk_space_f == 0 )
	    bfsk_space_f = bfsk_mark_f - autodetect_shift;
	if ( band_width == 0 )
	    band_width = 50;	// close enough
    } else {
	/*
	 * RTTY:     baud=45.45 mark/space=variable shift=-170
	 */
	autodetect_shift = 170;
	if ( bfsk_mark_f == 0 )
	    bfsk_mark_f  = 1585;
	if ( bfsk_space_f == 0 )
	    bfsk_space_f = bfsk_mark_f - autodetect_shift;
	if ( band_width == 0 ) {
	    band_width = 10;	// FIXME chosen arbitrarily
	}
    }

    if ( bfsk_txstopbits == 0 )
	bfsk_txstopbits = 1.0;

    /* restrict band_width to <= data rate (FIXME?) */
    if ( band_width > bfsk_data_rate )
	band_width = bfsk_data_rate;

    // sanitize confidence search limit
    if ( fsk_confidence_search_limit < fsk_confidence_threshold )
	fsk_confidence_search_limit = fsk_confidence_threshold;

    char *stream_name = NULL;;

    if ( filename ) {
	sa_backend = SA_BACKEND_FILE;
	stream_name = filename;
    }

    /*
     * Handle transmit mode
     */
    if ( TX_mode ) {

	simpleaudio_tone_init(tx_sin_table_len);

	int tx_interactive = 0;
	if ( ! stream_name ) {
	    tx_interactive = 1;
	    stream_name = "output audio";
	}

	simpleaudio *sa_out;
	sa_out = simpleaudio_open_stream(sa_backend, SA_STREAM_PLAYBACK,
					sample_format, sample_rate, nchannels,
					program_name, stream_name);
	if ( ! sa_out )
	    return 1;

	fsk_transmit_stdin(sa_out, tx_interactive,
				bfsk_data_rate,
				bfsk_mark_f, bfsk_space_f,
				bfsk_n_data_bits,
				bfsk_txstopbits,
				bfsk_framebits_encode
				);

	simpleaudio_close(sa_out);

	return 0;
    }


    /*
     * Open the input audio stream
     */

    if ( ! stream_name )
	stream_name = "input audio";

    simpleaudio *sa;
    sa = simpleaudio_open_stream(sa_backend, SA_STREAM_RECORD,
				sample_format, sample_rate, nchannels,
				program_name, stream_name);
    if ( ! sa )
        return 1;


    /*
     * Prepare the input sample chunk rate
     */
    float nsamples_per_bit = sample_rate / bfsk_data_rate;


    /*
     * Prepare the fsk plan
     */

    fsk_plan *fskp;
    fskp = fsk_plan_new(sample_rate, bfsk_mark_f, bfsk_space_f,
				band_width, bfsk_n_data_bits);
    if ( !fskp ) {
        fprintf(stderr, "fsk_plan_new() failed\n");
        return 1;
    }

    /*
     * Prepare the input sample buffer.  For 8-bit frames with prev/start/stop
     * we need 11 data-bits worth of samples, and we will scan through one bits
     * worth at a time, hence we need a minimum total input buffer size of 12
     * data-bits.  */
// FIXME I should be able to reduce this to * 9 for 5-bit data, but
// it SOMETIMES crashes -- probably due to non-integer nsamples_per_bit
// FIXME by passing it down into the fsk code?
    // FIXME EXPLAIN +1 goes with extra bit when scanning
    size_t	samplebuf_size = ceilf(nsamples_per_bit) * (12+1);
    float	*samplebuf = malloc(samplebuf_size * sizeof(float));
    float	*samples_readptr = samplebuf;
    size_t	read_nsamples = samplebuf_size;
    size_t	samples_nvalid = 0;
    debug_log("samplebuf_size=%zu\n", samplebuf_size);

    /*
     * Run the main loop
     */

    int			ret = 0;

    int			carrier = 0;
    float		confidence_total = 0;
    unsigned int	nframes_decoded = 0;
    size_t		carrier_nsamples = 0;

    unsigned int	noconfidence = 0;
    unsigned int	advance = 0;

    // Fraction of nsamples_per_bit that we will "overscan"; range (0.0 .. 1.0)
    float fsk_frame_overscan = 0.5;
    //   should be != 0.0 (only the nyquist edge cases actually require this?)
    // for handling of slightly faster-than-us rates:
    //   should be >> 0.0 to allow us to lag back for faster-than-us rates
    //   should be << 1.0 or we may lag backwards over whole bits
    // for optimal analysis:
    //   should be >= 0.5 (half a bit width) or we may not find the optimal bit
    //   should be <  1.0 (a full bit width) or we may skip over whole bits
    assert( fsk_frame_overscan >= 0.0 && fsk_frame_overscan < 1.0 );

    // ensure that we overscan at least a single sample
    unsigned int nsamples_overscan
			= nsamples_per_bit * fsk_frame_overscan + 0.5;
    if ( fsk_frame_overscan > 0.0 && nsamples_overscan == 0 )
	nsamples_overscan = 1;
    debug_log("fsk_frame_overscan=%f nsamples_overscan=%u\n",
	    fsk_frame_overscan, nsamples_overscan);

    while ( 1 ) {

	debug_log("advance=%u\n", advance);

	/* Shift the samples in samplebuf by 'advance' samples */
	assert( advance <= samplebuf_size );
	if ( advance == samplebuf_size ) {
	    samples_nvalid = 0;
	    samples_readptr = samplebuf;
	    read_nsamples = samplebuf_size;
	    advance = 0;
	}
	if ( advance ) {
	    if ( advance > samples_nvalid )
		break;
	    memmove(samplebuf, samplebuf+advance,
		    (samplebuf_size-advance)*sizeof(float));
	    samples_nvalid -= advance;
	    samples_readptr = samplebuf + (samplebuf_size-advance);
	    read_nsamples = advance;
	}

	/* Read more samples into samplebuf (fill it) */
	assert ( read_nsamples > 0 );
	assert ( samples_nvalid + read_nsamples <= samplebuf_size );
	ssize_t r;
	r = simpleaudio_read(sa, samples_readptr, read_nsamples);
	debug_log("simpleaudio_read(samplebuf+%zd, n=%zu) returns %zd\n",
		samples_readptr - samplebuf, read_nsamples, r);
	if ( r < 0 ) {
	    fprintf(stderr, "simpleaudio_read: error\n");
	    ret = -1;
            break;
	}
	else if ( r > 0 )
	    samples_nvalid += r;

	if ( samples_nvalid == 0 )
	    break;

	/* Auto-detect carrier frequency */
	static int carrier_band = -1;
	if ( carrier_autodetect_threshold > 0.0 && carrier_band < 0 ) {
	    unsigned int i;
	    float nsamples_per_scan = nsamples_per_bit;
	    if ( nsamples_per_scan > fskp->fftsize )
		nsamples_per_scan = fskp->fftsize;
	    for ( i=0; i+nsamples_per_scan<=samples_nvalid;
						 i+=nsamples_per_scan ) {
		carrier_band = fsk_detect_carrier(fskp,
				    samplebuf+i, nsamples_per_scan,
				    carrier_autodetect_threshold);
		if ( carrier_band >= 0 )
		    break;
	    }
	    advance = i + nsamples_per_scan;
	    if ( advance > samples_nvalid )
		advance = samples_nvalid;
	    if ( carrier_band < 0 ) {
		debug_log("autodetected carrier band not found\n");
		continue;
	    }

	    // FIXME: hardcoded negative shift
	    int b_shift = - (float)(autodetect_shift + fskp->band_width/2.0)
						/ fskp->band_width;
	    /* only accept a carrier as b_mark if it will not result
	     * in a b_space band which is "too low". */
	    if ( carrier_band + b_shift < 1 ) {
		debug_log("autodetected space band too low\n" );
		carrier_band = -1;
		continue;
	    }

	    debug_log("### TONE freq=%.1f ###\n",
		    carrier_band * fskp->band_width);

	    fsk_set_tones_by_bandshift(fskp, /*b_mark*/carrier_band, b_shift);
	}

	/*
	 * The main processing algorithm: scan samplesbuf for FSK frames,
	 * looking at an entire frame at once.
	 */

	debug_log( "--------------------------\n");

	unsigned int frame_nsamples = nsamples_per_bit * fskp->n_frame_bits;

	if ( samples_nvalid < frame_nsamples )
	    break;

	// try_max_nsamples = nsamples_per_bit + nsamples_overscan;
	// serves two purposes
	// 1. avoids finding a non-optimal first frame
	// 2. allows us to track slightly slow signals
	unsigned int try_max_nsamples = nsamples_per_bit + nsamples_overscan;

#define FSK_ANALYZE_NSTEPS		10	/* accuracy vs. performance */
		// Note: FSK_ANALYZE_NSTEPS has subtle effects on the
		// "rate perfect" calculation.  oh well.
	unsigned int try_step_nsamples = nsamples_per_bit / FSK_ANALYZE_NSTEPS;
	if ( try_step_nsamples == 0 )
	    try_step_nsamples = 1;

	float confidence;
	unsigned int bits = 0;
	/* Note: frame_start_sample is actually the sample where the
	 * prev_stop bit begins (since the "frame" includes the prev_stop). */
	unsigned int frame_start_sample = 0;

	// If we don't have carrier, then set this try_confidence_search_limit
	// to infinity (search for best possible frame) so to get the decoder
	// into phase with the signal, so the next try_first_sample will match
	// up with where the next frame should be.
	unsigned int try_first_sample;
	float try_confidence_search_limit;
	if ( carrier ) {
	    try_first_sample = nsamples_overscan;
	    try_confidence_search_limit = fsk_confidence_search_limit;
	} else {
	    try_first_sample = 0;
	    try_confidence_search_limit = INFINITY;
	}

	confidence = fsk_find_frame(fskp, samplebuf, frame_nsamples,
			try_first_sample,
			try_max_nsamples,
			try_step_nsamples,
			try_confidence_search_limit,
			&bits,
			&frame_start_sample
			);

	// FIXME: hardcoded chop off framing bits
	if ( fskp->n_data_bits == 5 )
	    bits = ( bits >> 2 ) & 0x1F;
	else
	    bits = ( bits >> 2 ) & 0xFF;

#define FSK_MAX_NOCONFIDENCE_BITS	20

	if ( confidence <= fsk_confidence_threshold ) {

	    // FIXME: explain
	    if ( ++noconfidence > FSK_MAX_NOCONFIDENCE_BITS )
	    {
		carrier_band = -1;
		if ( carrier ) {
		    if ( !quiet_mode )
			report_no_carrier(fskp, sample_rate, bfsk_data_rate,
			    nsamples_per_bit, nframes_decoded,
			    carrier_nsamples, confidence_total);
		    carrier = 0;
		    carrier_nsamples = 0;
		    confidence_total = 0;
		    nframes_decoded = 0;
		}
	    }

	    /* Advance the sample stream forward by try_max_nsamples so the
	     * next time around the loop we continue searching from where
	     * we left off this time.		*/
	    advance = try_max_nsamples;
	    debug_log("@ NOCONFIDENCE=%u advance=%u\n", noconfidence, advance);
	    continue;
	}

	// Add a frame's worth of samples to the sample count
	carrier_nsamples += nsamples_per_bit * (fskp->n_data_bits + 2);

	if ( carrier ) {

	    // If we already had carrier, adjust sample count +start -overscan
	    carrier_nsamples += frame_start_sample;
	    carrier_nsamples -= nsamples_overscan;

	} else {
	    if ( !quiet_mode ) {
		if ( bfsk_data_rate >= 100 )
		    fprintf(stderr, "### CARRIER %u @ %.1f Hz ###\n",
			    (unsigned int)(bfsk_data_rate + 0.5),
			    fskp->b_mark * fskp->band_width);
		else
		    fprintf(stderr, "### CARRIER %.2f @ %.1f Hz ###\n",
			    bfsk_data_rate,
			    fskp->b_mark * fskp->band_width);
	    }
	    carrier = 1;
	    bfsk_framebits_decode(0, 0, 0);	/* reset the frame processor */
	}


	confidence_total += confidence;
	nframes_decoded++;
	noconfidence = 0;

	/* Advance the sample stream forward past the decoded frame
	 * but not past the stop bit, since we want it to appear as
	 * the prev_stop bit of the next frame, so ...
	 *
	 * advance = 1 prev_stop + 1 start + N data bits == n_data_bits+2
	 *
	 * but actually advance just a bit less than that to allow
	 * for tracking slightly fast signals, hence - nsamples_overscan.
	 */
	advance = frame_start_sample
		+ nsamples_per_bit * (float)(fskp->n_data_bits + 2)
		- nsamples_overscan;

	debug_log("@ nsamples_per_bit=%.3f n_data_bits=%u "
			" frame_start=%u advance=%u\n",
		    nsamples_per_bit, fskp->n_data_bits,
		    frame_start_sample, advance);


	/*
	 * Send the raw data frame bits to the backend frame processor
	 * for final conversion to output data bytes.
	 */

	unsigned int dataout_size = 4096;
	char dataoutbuf[4096];
	unsigned int dataout_nbytes = 0;

	dataout_nbytes += bfsk_framebits_decode(dataoutbuf + dataout_nbytes,
						dataout_size - dataout_nbytes,
						bits);

	/*
	 * Print the output buffer to stdout
	 */
	if ( dataout_nbytes ) {
	    char *p = dataoutbuf;
	    for ( ; dataout_nbytes; p++,dataout_nbytes-- ) {
		char printable_char = isprint(*p)||isspace(*p) ? *p : '.';
		printf( "%c", printable_char );
	    }
	    fflush(stdout);
	}

    } /* end of the main loop */

    if ( carrier ) {
	if ( !quiet_mode )
	    report_no_carrier(fskp, sample_rate, bfsk_data_rate,
		nsamples_per_bit, nframes_decoded,
		carrier_nsamples, confidence_total);
    }

    simpleaudio_close(sa);

    fsk_plan_destroy(fskp);

    return ret;
}
