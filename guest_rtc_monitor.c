/*
 * Copyright (C) 2020 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>

#define NANOSECONDS_PER_SECOND  1000000000LL
#define DELTA_TIME              100000LL

const char *cmd_rtc = "{\"execute\": \"rtc-refresh-timer\"}";

struct timespec real0, real1, boot0, boot1;
int64_t expire;

/* Return true if the timer has already expired. */
bool handle_timer_cancellation(int64_t expire)
{
    uint64_t old, new;

    clock_gettime(CLOCK_BOOTTIME, &boot1);
    clock_gettime(CLOCK_REALTIME, &real1);
    old = real0.tv_sec * NANOSECONDS_PER_SECOND + real0.tv_nsec;
    old += boot1.tv_sec * NANOSECONDS_PER_SECOND + boot1.tv_nsec - boot0.tv_sec
        * NANOSECONDS_PER_SECOND - boot0.tv_nsec;
    new = real1.tv_sec * NANOSECONDS_PER_SECOND + real1.tv_nsec;

    boot0 = boot1;
    real0 = real1;

    /* Sometimes we are interrupted from the timer while host system time
     * barely changes. Only warn if there's significant (> 0.1ms) delta T. */
    if (new > old && new - old > DELTA_TIME) {
        printf("WARNING: Host realtime clock is changed, Guest RTC will change accordingly\n");
        printf("WARNING: Realtime clock jumped %lu ns\n", new - old);
    } else if (old > new && old - new > DELTA_TIME) {
        printf("WARNING: Host realtime clock is changed, Guest RTC will change accordingly\n");
        printf("WARNING: Realtime clock jumped -%lu ns\n", old - new);
    }

    return expire + DELTA_TIME < new;
}

bool setup_timer(int timerfd, int64_t expire, FILE *in)
{
    struct itimerspec ts;

    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec = expire / NANOSECONDS_PER_SECOND;
    ts.it_value.tv_nsec = expire % NANOSECONDS_PER_SECOND;

    while (timerfd_settime(timerfd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &ts, NULL) < 0) {
        if (errno != ECANCELED) {
            printf("Failed to setup alarm\n");
            return false;
        }
        if (handle_timer_cancellation(expire)) {
            printf("Guest RTC alarm expired, kick QEMU\n");
            fputs(cmd_rtc, in);
            fflush(in);
            return true;
        }
    }

    printf("Host RTC alarm is set to reflect Guest RTC alarm, will expire in %lu seconds\n",
                ts.it_value.tv_sec - time(NULL));
    return true;
}

int handle_qmp_cmd(char *cmd, int timerfd, FILE *in)
{
    json_object *jobj = json_tokener_parse(cmd);
    json_object *jevt;
    const char *event;
    if (!json_object_object_get_ex(jobj, "event", &jevt))
        return 0;
    event = json_object_get_string(jevt);

    if (strcmp(event, "RTC_CHANGE") == 0) {
        json_object *jdata, *joffset;
        int64_t delta;
        char cmd[256];

        /* NOTE: We must only sync Host RTC with Guest RTC, but not host system
         * time (i.e. gtod time), so that QEMU rtc device would not go crazy. */
        json_object_object_get_ex(jobj, "data", &jdata);
        json_object_object_get_ex(jdata, "offset", &joffset);
        delta = json_object_get_int64(joffset);
        snprintf(cmd, sizeof(cmd), "hwclock --set --date \"`date --date=\"@%ld\" +%%F\\ %%T`\"", time(NULL) + delta);
        if (system(cmd) < 0)
            printf("Failed to set Host RTC\n");
        else
            printf("Host RTC is updated, offset from original value is %ld seconds\n", delta);
    }
    else if (strcmp(event, "RTC_ALARM") == 0) {
        json_object *jdata, *jexpire;

        json_object_object_get_ex(jobj, "data", &jdata);
        json_object_object_get_ex(jdata, "expire", &jexpire);
        expire = json_object_get_int64(jexpire);
        if (!setup_timer(timerfd, expire, in))
            return 1;
    }
    else if (strcmp(event, "SHUTDOWN") == 0) {
        /* When guest is shutdown, we have a chance to sync host system time,
         * since it's impossible to break guest RTC now. */
        if (system("hwclock --hctosys") < 0)
            printf("Failed to set Host realtime clock\n");
        else
            printf("Host realtime clock is synced with Host RTC\n");
        json_object_put(jobj);
        return 1;
    }

    json_object_put(jobj);
    return 0;
}

int main(int argc, char *argv[])
{
    FILE *in, *out;
    int outfd, timerfd;
    struct pollfd ctxt[2];

    if (argc < 3) {
        printf("QMP pipe files not provided\n");
        return -1;
    }

    in = fopen(argv[1], "w");
    out = fopen(argv[2], "r");
    if (!in || !out) {
        printf("Failed to open QMP pipe\n");
        return -1;
    }
    outfd = fileno(out);

    timerfd = timerfd_create(CLOCK_REALTIME_ALARM, 0);
    if (timerfd < 0) {
        printf("Failed to create alarm\n");
        return -1;
    }

    ctxt[0].fd = outfd;
    ctxt[0].events = POLLIN;
    ctxt[1].fd = timerfd;
    ctxt[1].events = POLLIN;

    clock_gettime(CLOCK_BOOTTIME, &boot0);
    clock_gettime(CLOCK_REALTIME, &real0);
    expire = 0;

    while (1) {
        if (poll(ctxt, 2, -1) < 0)
            goto err;
        if (ctxt[0].revents & POLLIN) {
            size_t n = 0;
            char *line = NULL;
            if (getline(&line, &n, out) < 0)
                goto err;
            if (handle_qmp_cmd(line, timerfd, in))
                break;
        }
        if (ctxt[1].revents & POLLIN) {
            char buf[8];
            if (read(timerfd, buf, 8) < 0) {
                if (errno != ECANCELED)
                    goto err;

                if (!handle_timer_cancellation(expire)) {
                    printf("Alarm timer interrupted, setup the alarm again\n");
                    if (setup_timer(timerfd, expire, in))
                        continue;
                }
            }
            printf("Guest RTC alarm expired, kick QEMU\n");
            fputs(cmd_rtc, in);
            fflush(in);
        }
        if (ctxt[0].revents & POLLERR || ctxt[1].revents & POLLERR)
            printf("WARNING: POLLERR on %s\n", (ctxt[0].revents & POLLERR) ? "pipefd" : "timerfd");
    }

    return 0;

err:
    printf("Unexpected error!\n");
    return -1;
}
