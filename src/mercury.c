/*
 * mercury.c
 *
 * main() file for mercury packet metadata capture and analysis tool
 *
 * Copyright (c) 2019 Cisco Systems, Inc. All rights reserved.  License at
 * https://github.com/cisco/mercury/blob/master/LICENSE
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <thread>

#include "mercury.h"
#include "pcap_file_io.h"
#include "af_packet_v3.h"
#include "pcap_reader.h"
#include "analysis.h"
#include "signal_handling.h"
#include "config.h"
#include "output.h"
#include "license.h"
#include "version.h"
#include "rnd_pkt_drop.h"

#ifndef  MERCURY_SEMANTIC_VERSION
#warning MERCURY_SEMANTIC_VERSION is not defined
#define  MERCURY_SEMANTIC_VERSION 0,0,0
#endif

struct semantic_version mercury_version(MERCURY_SEMANTIC_VERSION);

char mercury_help[] =
    "%s [INPUT] [OUTPUT] [OPTIONS]:\n"
    "INPUT\n"
    "   [-c or --capture] capture_interface   # capture packets from interface\n"
    "   [-r or --read] read_file              # read packets from file\n"
    "   no input option                       # read packets from standard input\n"
    "OUTPUT\n"
    "   [-f or --fingerprint] json_file_name  # write JSON fingerprints to file\n"
    "   [-w or --write] pcap_file_name        # write packets to PCAP/MCAP file\n"
    "   no output option                      # write JSON fingerprints to stdout\n"
    "--capture OPTIONS\n"
    "   [-b or --buffer] b                    # set RX_RING size to (b * PHYS_MEM)\n"
    "   [-t or --threads] [num_threads | cpu] # set number of threads\n"
    "   [-u or --user] u                      # set UID and GID to those of user u\n"
    "   [-d or --directory] d                 # set working directory to d\n"
    "GENERAL OPTIONS\n"
    "   --config c                            # read configuration from file c\n"
    "   [-a or --analysis]                    # analyze fingerprints\n"
    "   --resources d                         # use resource directory d\n"
    "   [-s or --select] filter               # select traffic by filter (see --help)\n"
    "   --nonselected-tcp-data                # tcp data for nonselected traffic\n"
    "   --nonselected-udp-data                # udp data for nonselected traffic\n"
    "   [-l or --limit] l                     # rotate output file after l records\n"
    "   --dns-json                            # output DNS as JSON, not base64\n"
    "   --certs-json                          # output certs as JSON, not base64\n"
    "   --metadata                            # output more protocol metadata in JSON\n"
    "   [-v or --verbose]                     # additional information sent to stderr\n"
    "   --license                             # write license information to stdout\n"
    "   --version                             # write version information to stdout\n"
    "   [-h or --help]                        # extended help, with examples\n";

char mercury_extended_help[] =
    "\n"
    "DETAILS\n"
    "   \"[-c or --capture] c\" captures packets from interface c with Linux AF_PACKET\n"
    "   using a separate ring buffer for each worker thread.  \"[-t or --thread] t\"\n"
    "   sets the number of worker threads to t, if t is a positive integer; if t is\n"
    "   \"cpu\", then the number of threads will be set to the number of available\n"
    "   processors.  \"[-b or --buffer] b\" sets the total size of all ring buffers to\n"
    "   (b * PHYS_MEM) where b is a decimal number between 0.0 and 1.0 and PHYS_MEM\n"
    "   is the available memory; USE b < 0.1 EXCEPT WHEN THERE ARE GIGABYTES OF SPARE\n"
    "   RAM to avoid OS failure due to memory starvation.\n"
    "\n"
    "   \"[-f or --fingerprint] f\" writes a JSON record for each fingerprint observed,\n"
    "   which incorporates the flow key and the time of observation, into the file f.\n"
    "   With [-a or --analysis], fingerprints and destinations are analyzed and the\n"
    "   results are included in the JSON output.\n"
    "\n"
    "   \"[-w or --write] w\" writes packets to the file w, in PCAP format.  With the\n"
    "   option [-s or --select], packets are filtered so that only ones with\n"
    "   fingerprint metadata are written.\n"
    "\n"
    "   \"[r or --read] r\" reads packets from the file r, in PCAP format.\n"
    "\n"
    "   if neither -r nor -c is specified, then packets are read from standard input,\n"
    "   in PCAP format.\n"
    "\n"
    "   \"[-s or --select] f\" selects packets according to the metadata filter f, which\n"
    "   is a comma-separated list of the following strings:\n"
    "      dhcp          DHCP discover message\n"
    "      dns           DNS messages\n"
    "      tls           DTLS clientHello, serverHello, and certificates\n"
    "      http          HTTP request and response\n"
    "      ssh           SSH handshake and KEX\n"
    "      tcp           TCP headers\n"
    "      tcp.message   TCP initial message\n"
    "      tls           TLS clientHello, serverHello, and certificates\n"
    "      wireguard     WG handshake initiation message\n"
    "      all           all of the above\n"
    "      <no option>   all of the above\n"
    "      none          none of the above\n"
    "\n"
    "   --nonselected-tcp-data writes the first TCP Data field in a flow with\n"
    "   nonzero length, for *non*-selected traffic, into JSON.  This option provides\n"
    "   a view into the TCP data that the --select option does not recognize. The\n"
    "   --select filter affects the TCP data written by this option; use\n"
    "   '--select=none' to obtain the TCP data for each flow.\n"
    "\n"
    "   --nonselected-udp-data writes the first UDP Data field in a flow with\n"
    "   nonzero length, for *non*-selected traffic, into JSON.  This option provides\n"
    "   a view into the UDP data that the --select option does not recognize. The\n"
    "   --select filter affects the UDP data written by this option; use\n"
    "   '--select=none' to obtain the UDP data for each flow.\n"
    "\n"
    "   \"[-u or --user] u\" sets the UID and GID to those of user u, so that\n"
    "   output file(s) are owned by this user.  If this option is not set, then\n"
    "   the UID is set to SUDO_UID, so that privileges are dropped to those of\n"
    "   the user that invoked sudo.  A system account with username mercury is\n"
    "   created for use with a mercury daemon.\n"
    "\n"
    "   \"[-d or --directory] d\" sets the working directory to d, so that all output\n"
    "   files are written into that location.  When capturing at a high data rate, a\n"
    "   high performance filesystem and disk should be used, and NFS partitions\n"
    "   should be avoided.\n"
    "\n"
    "   \"--config c\" reads configuration information from the file c.\n"
    "\n"
    "   [-a or --analysis] performs analysis and reports results in the \"analysis\"\n"
    "   object in the JSON records.   This option only works with the option\n"
    "   [-f or --fingerprint].\n"
    "\n"
    "   \"[-l or --limit] l\" rotates output files so that each file has at most\n"
    "   l records or packets; filenames include a sequence number, date and time.\n"
    "\n"
    "   --dns-json writes out DNS responses as a JSON object; otherwise,\n"
    "   that data is output in base64 format, as a string with the key \"base64\".\n"
    "\n"
    "   --certs-json writes out certificates as JSON objects; otherwise,\n"
   "    that data is output in base64 format, as a string with the key \"base64\".\n"
    "\n"
    "   --metadata writes out additional metadata into the protocol JSON objects.\n"
    "\n"
    "   [-v or --verbose] writes additional information to the standard error,\n"
    "   including the packet count, byte count, elapsed time and processing rate, as\n"
    "   well as information about threads and files.\n"
    "\n"
    "   --license and --version write their information to stdout, then halt.\n"
    "\n"
    "   [-h or --help] writes this extended help message to stdout.\n"
    "\n"
    "SYSTEM\n"
    "   Resource files used in analysis: " DEFAULT_RESOURCE_DIR "\n"    // can be set via ./configure
    "   Systemd service output:          /usr/local/var/mercury\n"
    "   Systemd service configuration    /etc/mercury/mercury.cfg\n"
    "\n"
    "EXAMPLES\n"
    "   mercury -c eth0 -w foo.pcap           # capture from eth0, write to foo.pcap\n"
    "   mercury -c eth0 -w foo.pcap -t cpu    # as above, with one thread per CPU\n"
    "   mercury -c eth0 -w foo.mcap -t cpu -s # as above, selecting packet metadata\n"
    "   mercury -r foo.mcap -f foo.json       # read foo.mcap, write fingerprints\n"
    "   mercury -r foo.mcap -f foo.json -a    # as above, with fingerprint analysis\n"
    "   mercury -c eth0 -t cpu -f foo.json -a # capture and analyze fingerprints\n";


enum extended_help {
    extended_help_off = 0,
    extended_help_on  = 1
};

void usage(const char *progname, const char *err_string, enum extended_help extended_help) {
    if (err_string) {
        printf("error: %s\n", err_string);
    }
    printf(mercury_help, progname);
    if (extended_help) {
        printf("%s", mercury_extended_help);
    }
    exit(EXIT_FAILURE);
}

bool option_is_valid(const char *opt) {
    if (opt == NULL) {
        return false;
    }
    if (opt[0] == '-') {
        return false;  // appears to be in -x or --x format
    }
    return true;
}

extern struct global_variables global_vars;  /* defined in config.c */

int main(int argc, char *argv[]) {
    struct mercury_config cfg = mercury_config_init();

    while(1) {
        enum opt { config=1, version=2, license=3, dns_json=4, certs_json=5, metadata=6, resources=7, tcp_init_data=8, udp_init_data=9 };
        int opt_idx = 0;
        static struct option long_opts[] = {
            { "config",      required_argument, NULL, config  },
            { "resources",   required_argument, NULL, resources },
            { "version",     no_argument,       NULL, version },
            { "license",     no_argument,       NULL, license },
            { "dns-json",    no_argument,       NULL, dns_json },
            { "certs-json",  no_argument,       NULL, certs_json },
            { "metadata",    no_argument,       NULL, metadata },
            { "nonselected-tcp-data", no_argument, NULL, tcp_init_data },
            { "nonselected-udp-data", no_argument, NULL, udp_init_data },
            { "read",        required_argument, NULL, 'r' },
            { "write",       required_argument, NULL, 'w' },
            { "directory",   required_argument, NULL, 'd' },
            { "capture",     required_argument, NULL, 'c' },
            { "fingerprint", required_argument, NULL, 'f' },
            { "analysis",    no_argument,       NULL, 'a' },
            { "threads",     required_argument, NULL, 't' },
            { "buffer",      required_argument, NULL, 'b' },
            { "limit",       required_argument, NULL, 'l' },
            { "user",        required_argument, NULL, 'u' },
            { "help",        no_argument,       NULL, 'h' },
            { "select",      optional_argument, NULL, 's' },
            { "verbose",     no_argument,       NULL, 'v' },
            { NULL,          0,                 0,     0  }
        };
        int c = getopt_long(argc, argv, "r:w:c:f:t:b:l:u:s::oham:vp:d:", long_opts, &opt_idx);
        if (c < 0) {
            break;
        }
        switch(c) {
        case config:
            if (option_is_valid(optarg)) {
                mercury_config_read_from_file(&cfg, optarg);
            } else {
                usage(argv[0], "option config requires filename argument", extended_help_off);
            }
            break;
        case resources:
            if (option_is_valid(optarg)) {
                cfg.resources = optarg;
            } else {
                usage(argv[0], "option resources requires directory argument", extended_help_off);
            }
            break;
        case version:
            mercury_version.print(stdout);
            return EXIT_SUCCESS;
            break;
        case license:
            printf("%s\n", license_string);
            return EXIT_SUCCESS;
            break;
        case dns_json:
            if (optarg) {
                usage(argv[0], "option dns-json does not use an argument", extended_help_off);
            } else {
                global_vars.dns_json_output = true;
            }
            break;
        case certs_json:
            if (optarg) {
                usage(argv[0], "option certs-json does not use an argument", extended_help_off);
            } else {
                global_vars.certs_json_output = true;
            }
            break;
        case metadata:
            if (optarg) {
                usage(argv[0], "option metadata does not use an argument", extended_help_off);
            } else {
                global_vars.metadata_output = true;
            }
            break;
        case tcp_init_data:
            if (optarg) {
                usage(argv[0], "option nonselected-tcp-data does not use an argument", extended_help_off);
            } else {
                global_vars.output_tcp_initial_data = true;
            }
            break;
        case udp_init_data:
            if (optarg) {
                usage(argv[0], "option nonselected-udp-data does not use an argument", extended_help_off);
            } else {
                global_vars.output_udp_initial_data = true;
            }
            break;
        case 'r':
            if (option_is_valid(optarg)) {
                cfg.read_filename = optarg;
            } else {
                usage(argv[0], "option r or read requires filename argument", extended_help_off);
            }
            break;
        case 'w':
            if (option_is_valid(optarg)) {
                cfg.write_filename = optarg;
            } else {
                usage(argv[0], "option w or write requires filename argument", extended_help_off);
            }
            break;
        case 'd':
            if (option_is_valid(optarg)) {
                cfg.working_dir = optarg;
            } else {
                usage(argv[0], "option d or directory requires working directory argument", extended_help_off);
            }
            break;
        case 'c':
            if (option_is_valid(optarg)) {
                cfg.capture_interface = optarg;
            } else {
                usage(argv[0], "option c or capture requires interface argument", extended_help_off);
            }
            break;
        case 'f':
            if (option_is_valid(optarg)) {
                cfg.fingerprint_filename = optarg;
            } else {
                usage(argv[0], "option f or fingerprint requires filename argument", extended_help_off);
            }
            break;
        case 'a':
            if (optarg) {
                usage(argv[0], "option a or analysis does not use an argument", extended_help_off);
            } else {
                cfg.analysis = true;
            }
            break;
        case 'o':
            if (optarg) {
                usage(argv[0], "option o or overwrite does not use an argument", extended_help_off);
            } else {
                /*
                 * remove 'exclusive' and add 'truncate' flags, to cause file writes to overwrite files if need be
                 */
                cfg.flags = O_TRUNC;
                /*
                 * set file mode similarly
                 */
                cfg.mode = (char *)"w";
            }
            break;
        case 's':
            if (optarg) {
                if (cfg.packet_filter_cfg != NULL) {
                    usage(argv[0], "option s or select used more than once", extended_help_off);
                }
                if (option_is_valid(optarg)) {
                    cfg.packet_filter_cfg = optarg;
                } else {
                    usage(argv[0], "option s or select has the form -s\"filter\" or --select=\"filter\"", extended_help_off);
                }
            }
            cfg.filter = 1;
            break;
        case 'h':
            if (optarg) {
                usage(argv[0], "option h or help does not use an argument", extended_help_on);
            } else {
                printf("mercury: packet metadata capture and analysis\n");
                usage(argv[0], NULL, extended_help_on);
            }
            break;
        case 'T':
            if (optarg) {
                usage(argv[0], "option T or test does not use an argument", extended_help_off);
            } else {
                cfg.use_test_packet = 1;
             }
            break;
        case 't':
            if (option_is_valid(optarg)) {
                if (strcmp(optarg, "cpu") == 0) {
                    cfg.num_threads = -1; /* create as many threads as there are cpus */
                    break;
                }
                errno = 0;
                cfg.num_threads = strtol(optarg, NULL, 10);
                if (cfg.num_threads == 0 || errno) {
                    printf("error: could not convert argument \"%s\" to a non-negative number\n", optarg);
                    usage(argv[0], "option t or threads requires a numeric argument", extended_help_off);
                }
            } else {
                usage(argv[0], "option t or threads requires a numeric argument", extended_help_off);
            }
            break;
        case 'l':
            if (option_is_valid(optarg)) {
                errno = 0;
                cfg.rotate = strtol(optarg, NULL, 10);
                if (errno) {
                    printf("%s: could not convert argument \"%s\" to a number\n", strerror(errno), optarg);
                }
            } else {
                usage(argv[0], "option l or limit requires a numeric argument", extended_help_off);
            }
            break;
        case 'p':
            if (option_is_valid(optarg)) {
                errno = 0;
                cfg.loop_count = strtol(optarg, NULL, 10);
                if (errno) {
                    printf("%s: could not convert argument \"%s\" to a number\n", strerror(errno), optarg);
                }
            } else {
                usage(argv[0], "option p or loop requires a numeric argument", extended_help_off);
            }
            break;
        case 0:
            /* The option --adaptive to adaptively accept or skip packets for PCAP file. */
            if (optarg) {
                usage(argv[0], "option --adaptive does not use an argument", extended_help_off);
            } else {
                cfg.adaptive = 1;
            }
            break;
        case 'u':
            if (option_is_valid(optarg)) {
                errno = 0;
                cfg.user = optarg;
            } else {
                usage(argv[0], "option u or user requires an argument", extended_help_off);
            }
            break;
        case 'b':
            if (option_is_valid(optarg)) {
                errno = 0;
                cfg.buffer_fraction = strtof(optarg, NULL);
                if (errno) {
                    printf("%s: could not convert argument \"%s\" to a number\n", strerror(errno), optarg);
                    usage(argv[0], NULL, extended_help_off);
                }
                if (cfg.buffer_fraction < 0.0 || cfg.buffer_fraction > 1.0 ) {
                    usage(argv[0], "buffer fraction must be between 0.0 and 1.0 inclusive", extended_help_off);
                }
            } else {
                usage(argv[0], "option b or buffer requires a numeric argument", extended_help_off);
            }
            break;
        case 'v':
            if (optarg) {
                usage(argv[0], "option v or verbose does not use an argument", extended_help_off);
            } else {
                cfg.verbosity = 1;
            }
            break;
        case '?':
        default:
            usage(argv[0], NULL, extended_help_off);
        }
    }
    if (optind < argc) {
        printf("unused options string(s): ");
        while (optind < argc) {
            printf("%s ", argv[optind++]);
        }
        printf("\n");
        usage(argv[0], "unrecognized options", extended_help_off);
    }

    if (cfg.read_filename == NULL && cfg.capture_interface == NULL) {
        cfg.read_filename = (char *)"-";  // convention: a dash indicates to read from stdin
    }
    if (cfg.read_filename != NULL && cfg.capture_interface != NULL) {
        usage(argv[0], "incompatible arguments read [r] and capture [c] specified on command line", extended_help_off);
    }
    if (cfg.fingerprint_filename && cfg.write_filename) {
        usage(argv[0], "both fingerprint [f] and write [w] specified on command line", extended_help_off);
    }

    if (cfg.read_filename) {
        cfg.output_block = true;      // use blocking output, so that no packets are lost in copying
    }

    if (cfg.analysis) {
        if (analysis_init(cfg.verbosity, cfg.resources) == -1) {
            return EXIT_FAILURE;  /* analysis engine could not be initialized */
        };
        global_vars.do_analysis = true;
    }

    /*
     * loop_count < 1  ==> not valid
     * loop_count > 1  ==> looping (i.e. repeating read file) will be done
     * loop_count == 1 ==> default condition
     */
    if (cfg.loop_count < 1) {
        usage(argv[0], "Invalid loop count, it should be >= 1", extended_help_off);
    } else if (cfg.loop_count > 1) {
        printf("Loop count: %d\n", cfg.loop_count);
    }

    /* The option --adaptive works only with -w PCAP file option and -c capture interface */
    if (cfg.adaptive > 0) {
        if (cfg.write_filename == NULL || cfg.capture_interface == NULL) {
            usage(argv[0], "The option --adaptive requires options -c capture interface and -w pcap file.", extended_help_off);
        } else {
            set_percent_accept(30); /* set starting percentage */
        }
    }

    /*
     * set up signal handlers, so that output is flushed upon close
     */
    if (setup_signal_handler() != status_ok) {
        fprintf(stderr, "%s: error while setting up signal handlers\n", strerror(errno));
    }

    /* set the number of threads, if needed */
    if (cfg.num_threads == -1) {
        int num_cpus = std::thread::hardware_concurrency();
        cfg.num_threads = num_cpus;
        if (cfg.verbosity) {
            fprintf(stderr, "found %d CPU(s), creating %d thread(s)\n", num_cpus, cfg.num_threads);
        }
    }

    /* init random number generator */
    srand(time(0));

    pthread_t output_thread;
    struct output_file out_file;
    if (output_thread_init(output_thread, out_file, cfg) != 0) {
        fprintf(stderr, "error: unable to initialize output thread\n");
        return EXIT_FAILURE;
    }
    if (cfg.capture_interface) {

        if (cfg.verbosity) {
            fprintf(stderr, "initializing interface %s\n", cfg.capture_interface);
        }
        if (bind_and_dispatch(&cfg, &out_file) != status_ok) {
            fprintf(stderr, "error: bind and dispatch failed\n");
            return EXIT_FAILURE;
        }
    } else if (cfg.read_filename) {

        if (open_and_dispatch(&cfg, &out_file) != status_ok) {
            return EXIT_FAILURE;
        }
    }

    if (cfg.analysis) {
        analysis_finalize();
    }

    if (cfg.verbosity) {
        fprintf(stderr, "stopping output thread and flushing queued output to disk.\n");
    }
    output_thread_finalize(output_thread, &out_file);

    return 0;
}
