/*
 * GPU Top
 *
 * Copyright (C) 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <getopt.h>
#include <unistd.h>

#include "gputop-client-context.h"
#include "gputop-network.h"

#include <uv.h>

static struct gputop_client_context ctx;
static const char *metric_name = NULL;

const struct gputop_metric_set_counter timestamp_counter = {
    .metric_set = NULL,
    .name = "Timestamp",
    .symbol_name = "Timestamp",
    .desc = "OA unit timestamp",
    .type = GPUTOP_PERFQUERY_COUNTER_TIMESTAMP,
    .data_type = GPUTOP_PERFQUERY_COUNTER_DATA_UINT64,
    .units = GPUTOP_PERFQUERY_COUNTER_UNITS_NS,
};
static struct {
    char *symbol_name;
    const struct gputop_metric_set_counter *counter;
    int width;
} *metric_columns = NULL;
static int n_metric_columns = 0;
static int n_accumulations = 0;
static struct gputop_accumulated_samples *last_samples;
static int current_pid = -1;
static bool human_units = true;
static bool print_headers = true;
static FILE *wrapper_output = NULL;

static void comment(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
}

static void output(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vfprintf(wrapper_output, format, ap);
    va_end(ap);
}

void gputop_cr_console_log(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static int unit_to_width(gputop_counter_units_t unit)
{
    switch (unit) {
    case GPUTOP_PERFQUERY_COUNTER_UNITS_BYTES:   return 8;
    case GPUTOP_PERFQUERY_COUNTER_UNITS_HZ:      return 8;
    case GPUTOP_PERFQUERY_COUNTER_UNITS_NS:      return 12;
    case GPUTOP_PERFQUERY_COUNTER_UNITS_US:      return 8;
    case GPUTOP_PERFQUERY_COUNTER_UNITS_PIXELS:
    case GPUTOP_PERFQUERY_COUNTER_UNITS_TEXELS:  return 8 + 2;
    case GPUTOP_PERFQUERY_COUNTER_UNITS_THREADS: return 8;
    case GPUTOP_PERFQUERY_COUNTER_UNITS_PERCENT: return 6;


    case GPUTOP_PERFQUERY_COUNTER_UNITS_MESSAGES:
    case GPUTOP_PERFQUERY_COUNTER_UNITS_NUMBER:
    case GPUTOP_PERFQUERY_COUNTER_UNITS_CYCLES:
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EVENTS:
    case GPUTOP_PERFQUERY_COUNTER_UNITS_UTILIZATION:
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EU_SENDS_TO_L3_CACHE_LINES:
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EU_ATOMIC_REQUESTS_TO_L3_CACHE_LINES:
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EU_REQUESTS_TO_L3_CACHE_LINES:
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EU_BYTES_PER_L3_CACHE_LINE:  return 8 + 2;

    default:
        assert(!"Missing case");
        return 0;
    }
}

static const char *unit_to_string(gputop_counter_units_t unit)
{
    switch (unit) {
    case GPUTOP_PERFQUERY_COUNTER_UNITS_BYTES:   return "B";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_HZ:      return "Hz";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_NS:      return "ns";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_US:      return "us";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_PIXELS:  return "pixels";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_TEXELS:  return "texels";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_THREADS: return "threads";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_PERCENT: return "%";


    case GPUTOP_PERFQUERY_COUNTER_UNITS_MESSAGES: return "messages/s";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_NUMBER:   return "/s";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_CYCLES:   return "cycles/s";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EVENTS:   return "events/s";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_UTILIZATION: return "utilization";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EU_SENDS_TO_L3_CACHE_LINES: return "sends-to-L3-CL";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EU_ATOMIC_REQUESTS_TO_L3_CACHE_LINES: return "atomics-to-L3-CL";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EU_REQUESTS_TO_L3_CACHE_LINES: return "requests-to-L3-CL";
    case GPUTOP_PERFQUERY_COUNTER_UNITS_EU_BYTES_PER_L3_CACHE_LINE:  return "bytes-per-L3-CL";

    default:
        assert(!"Missing case");
        return 0;
    }
}

static void quit(void)
{
    gputop_client_context_stop_sampling(&ctx);
    gputop_connection_close(ctx.connection);
}

static char **child_process_args = NULL;
static const char *child_process_output_file = "wrapper_child_output.txt";
static uint32_t n_child_process_args = 0;

static void start_child_process(void)
{
    int i, fd_out;

    if (ctx.devinfo.gen < 8) {
        comment("Process monitoring not supported in Haswell\n");
        quit();
        return;
    }

    current_pid = fork();
    switch (current_pid) {
    case 0:
        close(1);
        fd_out = open(child_process_output_file, O_CREAT | O_CLOEXEC,
                      S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        if (fd_out == -1) {
            comment("Cannot create output file '%s': %s\n",
                    child_process_output_file, strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (dup2(fd_out, 1) == -1) {
            comment("Error redirecting output stream: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        execvp(child_process_args[0], child_process_args);
        break;
    case -1:
        comment("Cannot start child process: %s\n", strerror(errno));
        quit();
        break;
    default:
        comment("Monitoring pid=%u: ", current_pid);
        for (i = 0; i < n_child_process_args; i++)
            comment("%s ", child_process_args[i]);
        comment("\n");

        break;
    }
}

static void on_ctrl_c(uv_signal_t* handle, int signum)
{
    quit();
}

static uv_timer_t child_process_timer_handle;
static int child_process_exit_accumulations;

static void on_child_timer(uv_timer_t* handle)
{
    uv_timer_stop(&child_process_timer_handle);
    if (n_accumulations > child_process_exit_accumulations) {
        quit();
    } else
        uv_timer_again(&child_process_timer_handle);
}

static void on_child_process_exit(uv_signal_t* handle, int signum)
{
    /* Given it another aggregation period and quit. */
    comment("Child exited.\n");
    child_process_exit_accumulations = n_accumulations;
    uv_timer_init(uv_default_loop(), &child_process_timer_handle);
    uv_timer_start(&child_process_timer_handle, on_child_timer,
                   ctx.oa_aggregation_period_ms, ctx.oa_aggregation_period_ms);
}

static void print_system_info(void)
{
    const struct gputop_devinfo *devinfo = &ctx.devinfo;
    char temp[80];

    comment("System info:\n");
    comment("\tKernel release: %s\n", ctx.features->features->kernel_release);
    comment("\tKernel build: %s\n", ctx.features->features->kernel_build);

    comment("CPU info:\n");
    comment("\tCPU model: %s\n", ctx.features->features->cpu_model);
    comment("\tCPU cores: %i\n", ctx.features->features->n_cpus);

    comment("GPU info:\n");
    comment("\tGT name: %s (Gen %u, PCI 0x%x)\n",
            devinfo->prettyname, devinfo->gen, devinfo->devid);
    comment("\tTopology: %llu threads, %llu EUs, %llu slices, %llu subslices\n",
            devinfo->eu_threads_count, devinfo->n_eus,
            devinfo->n_slices, devinfo->n_subslices);
    comment("\tGT frequency range: %.1fMHz / %.1fMHz\n",
            (double) devinfo->gt_min_freq / 1000000.0f,
            (double) devinfo->gt_max_freq / 1000000.0f);
    comment("\tCS timestamp frequency: %lu Hz / %.2f ns\n",
            devinfo->timestamp_frequency,
            1000000000.0f / devinfo->timestamp_frequency);

    comment("OA info:\n");
    comment("\tOA Hardware Sampling Exponent: %u\n",
            gputop_period_to_oa_exponent(&ctx, ctx.oa_aggregation_period_ms));
    gputop_client_pretty_print_value(GPUTOP_PERFQUERY_COUNTER_UNITS_NS,
                                     gputop_oa_exponent_to_period_ns(&ctx.devinfo,
                                                                     gputop_period_to_oa_exponent(&ctx,
                                                                                                  ctx.oa_aggregation_period_ms)),
                                     temp, sizeof(temp));
    comment("\tOA Hardware Period: %u ns / %s\n",
            gputop_oa_exponent_to_period_ns(&ctx.devinfo,
                                            gputop_period_to_oa_exponent(&ctx, ctx.oa_aggregation_period_ms)),
            temp);
}

static void print_metrics(void)
{
    struct hash_entry *entry;
    comment("List of metric sets selectable with -m/--metrics=...\n");
    hash_table_foreach(ctx.metrics_map, entry) {
        const struct gputop_metric_set *metric_set = entry->data;
        comment("\t%s: %s hw-config-guid=%s\n",
                metric_set->symbol_name, metric_set->name, metric_set->hw_config_guid);
    }
}

static void print_metric_counter(const struct gputop_metric_set *metric_set)
{
    int i, max_symbol_length = 0;
    comment("ALL: Timestamp");
    for (i = 0; i < metric_set->n_counters; i++) {
        comment(",%s", metric_set->counters[i].symbol_name);
        max_symbol_length = MAX2(strlen(metric_set->counters[i].symbol_name),
                                 max_symbol_length);
    }
    comment("\n\n");
    comment("Detailed:\n");
    for (i = 0; i < metric_set->n_counters; i++) {
        comment("%s:%*s %s\n",
                metric_set->counters[i].symbol_name,
                max_symbol_length - strlen(metric_set->counters[i].symbol_name), "",
                metric_set->counters[i].desc);
    }
}

static void print_metric_colum_names(void)
{
    int i;
    for (i = 0; i < n_metric_columns; i++) {
        output("%*s%s ",
               metric_columns[i].width - strlen(metric_columns[i].counter->symbol_name), "",
               metric_columns[i].counter->symbol_name);
    }
    output("\n");
    for (i = 0; i < n_metric_columns; i++) {
        const char *units = unit_to_string(metric_columns[i].counter->units);
        output("%*s(%s) ", metric_columns[i].width - strlen(units) - 2, "", units);
    }
    output("\n");
}

static bool match_process(struct gputop_hw_context *context)
{
    if (context == NULL) {
        if (current_pid == 0)
            return true;
        return false;
    }

    if (!context->process)
        return false;

    return context->process->pid == current_pid;
}

static void print_accumulated_columns(struct gputop_client_context *ctx,
                                      struct gputop_accumulated_samples *samples)
{
    int i;
    for (i = 0; i < n_metric_columns; i++) {
        const struct gputop_metric_set_counter *counter = metric_columns[i].counter;
        char svalue[20];

        if (counter == &timestamp_counter) {
            snprintf(svalue, sizeof(svalue), "%" PRIu64, samples->accumulator.first_timestamp);
        } else {
            double value = gputop_client_context_read_counter_value(ctx, samples, counter);
            if (human_units)
                gputop_client_pretty_print_value(counter->units, value, svalue, sizeof(svalue));
            else
                snprintf(svalue, sizeof(svalue), "%.2f", value);
        }
        output("%*s%s%s", metric_columns[i].width - strlen(svalue), "",
               svalue, i == (n_metric_columns - 1) ? "" : ",");
    }
    output("\n");
}

static void print_columns(struct gputop_client_context *ctx,
                          struct gputop_hw_context *context)
{
    struct list_head *list;

    if (!match_process(context))
        return;

    n_accumulations++;
    list = context == NULL ? &ctx->graphs : &context->graphs;

    if (last_samples == NULL) {
        list_for_each_entry(struct gputop_accumulated_samples, samples, list, link) {
            print_accumulated_columns(ctx, samples);
        }
        last_samples = list_last_entry(list, struct gputop_accumulated_samples, link);
    } else {
        last_samples = list_last_entry(list, struct gputop_accumulated_samples, link);
        print_accumulated_columns(ctx, last_samples);
    }
}

static bool handle_features()
{
    static bool info_printed = false;
    int i;

    if (!metric_name ||
        (ctx.metric_set = gputop_client_context_symbol_to_metric_set(&ctx, metric_name)) == NULL) {
        print_metrics();
        return true;
    }
    if (!metric_columns) {
        print_metric_counter(ctx.metric_set);
        return true;
    }
    if (!metric_columns[0].counter) {
        for (i = 0; i < n_metric_columns; i++) {
            int j;

            if (!strcmp("Timestamp", metric_columns[i].symbol_name)) {
                metric_columns[i].counter = &timestamp_counter;
            } else {
                for (j = 0; j < ctx.metric_set->n_counters; j++) {
                    if (!strcmp(ctx.metric_set->counters[j].symbol_name,
                                metric_columns[i].symbol_name)) {
                        metric_columns[i].counter = &ctx.metric_set->counters[j];
                        break;
                    }
                }

                if (!metric_columns[i].counter) {
                    comment("Unknown counter '%s'\n", metric_columns[i]);
                    return true;
                }
            }

            metric_columns[i].width = MAX3(strlen(metric_columns[i].counter->symbol_name),
                                           strlen(unit_to_string(metric_columns[i].counter->units)) + 2,
                                           unit_to_width(metric_columns[i].counter->units)) + 1;
        }
    }
    if (!info_printed && ctx.features) {
        info_printed = true;
        print_system_info();
    }
    if (print_headers)
        print_metric_colum_names();
    return false;
}

static void on_ready(gputop_connection_t *conn, void *user_data)
{
    comment("Connected\n\n");
    gputop_client_context_reset(&ctx, conn);
}

static void on_data(gputop_connection_t *conn,
                    const void *data, size_t len,
                    void *user_data)
{
    static bool features_handled = false;

    gputop_client_context_handle_data(&ctx, data, len);
    if (!features_handled && ctx.features) {
        features_handled = true;
        if (handle_features()) {
            quit();
            return;
        }
        if (current_pid != 0)
            gputop_client_context_add_tracepoint(&ctx, "i915/i915_gem_request_add");
        else
            gputop_client_context_start_sampling(&ctx);
    } else {
        if (!ctx.is_sampling) {
            bool all_tracepoints = true;
            list_for_each_entry(struct gputop_perf_tracepoint, tp,
                                &ctx.perf_tracepoints, link) {
                if (tp->event_id == 0)
                    all_tracepoints = false;
            }
            if (all_tracepoints)
                gputop_client_context_start_sampling(&ctx);
        } else {
            if (child_process_args && current_pid == -1)
                start_child_process();
        }
    }
}

static void on_close(gputop_connection_t *conn, const char *error,
                     void *user_data)
{
    if (error)
        comment("Connection error : %s\n", error);

    ctx.connection = NULL;
    uv_stop(uv_default_loop());
}

static const char *next_column(const char *string)
{
    const char *s;
    if (!string)
        return NULL;

    s = strchr(string, ',');
    if (s)
        return s + 1;
    return NULL;
}

static void usage(void)
{
    output("Usage: gputop-wrapper [options] <program> [program args...]\n"
           "\n"
           "\t -h, --help                        Display this help\n"
           "\t -H, --host <hostname>             Host to connect to\n"
           "\t -p, --port <port>                 Port on which the server is running\n"
           "\t -P, --period <period>             Accumulation period (in seconds, floating point)\n"
           "\t -m, --metric <name>               Metric set to use (printed out if this option is missing)\n"
           "\t -c, --columns <col0,col1,..>      Columns to print out (printed out if this option is missing)\n"
           "\t -n, --no-human-units              Disable human readable units (for machine readable output)\n"
           "\t -N, --no-headers                  Disable headers (for machine readable output)\n"
           "\t -O, --child-output <filename>     Outputs the child's standard output to filename\n"
           "\t -o, --output <filename>           Outputs gputop-wrapper's data to filename (disables human readable units)\n"
           "\n"
        );
}

int
main (int argc, char **argv)
{
    const struct option long_options[] = {
        { "help",            no_argument,        0, 'h' },
        { "host",            required_argument,  0, 'H' },
        { "port",            required_argument,  0, 'p' },
        { "period",          required_argument,  0, 'P' },
        { "metric",          required_argument,  0, 'm' },
        { "columns",         required_argument,  0, 'c' },
        { "no-human-units",  no_argument,        0, 'n' },
        { "no-headers",      no_argument,        0, 'N' },
        { "child-output",    required_argument,  0, 'O' },
        { "output",          required_argument,  0, 'o' },
        { NULL,              required_argument,  0, '-' },
        { 0, 0, 0, 0 }
    };
    int opt, port = 7890;
    const char *host = "localhost";
    bool opt_done = false;
    char temp[1024];
    uv_loop_t *loop;
    uv_signal_t ctrl_c_handle;
    uv_signal_t child_process_handle;

    gputop_client_context_init(&ctx);
    ctx.accumulate_cb = print_columns;
    ctx.oa_aggregation_period_ms = 1000;

    wrapper_output = stdout;

    while (!opt_done &&
           (opt = getopt_long(argc, argv, "c:hH:m:p:P:-nNO:o:", long_options, NULL)) != -1)
    {
        switch (opt) {
        case 'h':
            usage();
            return EXIT_SUCCESS;
        case 'H':
            host = optarg;
            break;
        case 'm':
            metric_name = optarg;
            break;
        case 'c': {
            const char *s = optarg;
            int n;
            n_metric_columns = 1;
            while ((s = next_column(s)) != NULL)
                n_metric_columns++;

            metric_columns = calloc(n_metric_columns, sizeof(metric_columns[0]));

            for (s = optarg, n = 0; s != NULL; s = next_column(s)) {
                metric_columns[n++].symbol_name = strndup(s, next_column(s) ?
                                                          (next_column(s) - s - 1) : strlen(s));
            }
            break;
        }
        case 'p':
            port = atoi(optarg);
            break;
        case 'P':
            ctx.oa_aggregation_period_ms = atof(optarg) * 1000.0f;
            break;
        case 'n':
            human_units = false;
            break;
        case 'N':
            print_headers = false;
            break;
        case 'O':
            child_process_output_file = optarg;
            break;
        case 'o':
            wrapper_output = fopen(optarg, "w+");
            human_units = false;
            if (wrapper_output == NULL) {
                comment("Unable to open output file '%s': %s\n",
                        optarg, strerror(errno));
                return EXIT_FAILURE;
            }
            break;
        case '-':
            opt_done = true;
            break;
        default:
            comment("Unrecognized option: %d\n", opt);
            return EXIT_FAILURE;
        }
    }

    loop = uv_default_loop();
    uv_signal_init(loop, &ctrl_c_handle);
    uv_signal_start_oneshot(&ctrl_c_handle, on_ctrl_c, SIGINT);

    uv_signal_init(loop, &child_process_handle);
    uv_signal_start_oneshot(&child_process_handle, on_child_process_exit, SIGCHLD);

    gputop_connect(host, port, on_ready, on_data, on_close, NULL);

    gputop_client_pretty_print_value(GPUTOP_PERFQUERY_COUNTER_UNITS_US,
                                     ctx.oa_aggregation_period_ms * 1000.0f,
                                     temp, sizeof(temp));
    comment("Server: %s:%i\n", host, port);
    comment("Sampling period: %s\n", temp);

    if (optind == argc) {
        comment("Monitoring: system wide\n");
        current_pid = 0;
    } else {
        if (strcmp(host, "localhost") != 0) {
            comment("Cannot monitor process on a different host.\n");
            return EXIT_FAILURE;
        }

        child_process_args = &argv[optind];
        n_child_process_args = argc - optind;
    }

    uv_run(loop, UV_RUN_DEFAULT);
    uv_signal_stop(&ctrl_c_handle);
    uv_signal_stop(&child_process_handle);

    gputop_client_context_reset(&ctx, NULL);

    comment("Finished.\n");

    return EXIT_SUCCESS;
}