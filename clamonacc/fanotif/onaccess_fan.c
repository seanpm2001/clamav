/*
 *  Copyright (C) 2019 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 *
 *  Authors: Mickey Sola
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#if defined(FANOTIFY)

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <sys/fanotify.h>

#include "../../libclamav/clamav.h"
#include "../../libclamav/scanners.h"

#include "../../shared/optparser.h"
#include "../../shared/output.h"

#include "../misc/onaccess_others.h"
#include "../../clamd/server.h"

#include "./onaccess_fan.h"
#include "../inotif/onaccess_hash.h"
#include "../inotif/onaccess_ddd.h"

#include "../client/onaccess_client.h"

#include "../scan/onaccess_scth.h"
#include "../scan/onaccess_scque.h"

extern pthread_t ddd_pid;
extern pthread_t scque_pid;
static int onas_fan_fd;

static void onas_fan_exit(int sig)
{
	logg("*ClamFanotif: onas_fan_exit(), signal %d\n", sig);

        if(onas_fan_fd) {
    close(onas_fan_fd);
        }
        onas_fan_fd = 0;

    if (ddd_pid > 0) {
        pthread_kill(ddd_pid, SIGUSR1);
        pthread_join(ddd_pid, NULL);
    }
        ddd_pid = 0;

        if (scque_pid > 0) {
		pthread_kill(scque_pid, SIGUSR2);
		pthread_join(scque_pid, NULL);
    }
        scque_pid = 0;

	logg("ClamFanotif: stopped\n");
	pthread_exit(NULL);
}

cl_error_t onas_setup_fanotif(struct onas_context **ctx) {

    const struct optstruct *pt;
    short int scan;
    unsigned int sizelimit = 0, extinfo;
	uint64_t fan_mask = FAN_EVENT_ON_CHILD;
    char err[128];

    pthread_attr_t ddd_attr;
    struct ddd_thrarg *ddd_tharg = NULL;

    ddd_pid = 0;

    /* Initialize fanotify */
    onas_fan_fd = fanotify_init(FAN_CLASS_CONTENT | FAN_UNLIMITED_QUEUE | FAN_UNLIMITED_MARKS, O_LARGEFILE | O_RDONLY);
    if (onas_fan_fd < 0) {
		logg("!ClamFanotif: fanotify_init failed: %s\n", cli_strerror(errno, err, sizeof(err)));
        if (errno == EPERM)
			logg("!ClamFanotif: clamonacc must have elevated permissions ... exiting ...\n");
		return CL_EOPEN;
    }


	if (!ctx || !*ctx) {
		logg("!ClamFanotif: unable to start clamonacc. (bad context)\n");
		return CL_EARG;
    }

	(*ctx)->fan_fd = onas_fan_fd;
	(*ctx)->fan_mask = fan_mask;

	if (optget((*ctx)->clamdopts, "OnAccessPrevention")->enabled && !optget((*ctx)->clamdopts, "OnAccessMountPath")->enabled) {
		logg("*ClamFanotif: kernel-level blocking feature enabled ... preventing malicious files access attempts\n");
		(*ctx)->fan_mask |= FAN_ACCESS_PERM | FAN_OPEN_PERM;
    } else {
		logg("*ClamFanotif: kernel-level blocking feature disabled ...\n");
		if (optget((*ctx)->clamdopts, "OnAccessPrevention")->enabled && optget((*ctx)->clamdopts, "OnAccessMountPath")->enabled) {
			logg("*ClamFanotif: feature not available when watching mounts ... \n");
		}
		(*ctx)->fan_mask |= FAN_ACCESS | FAN_OPEN;
    }

	if ((pt = optget((*ctx)->clamdopts, "OnAccessMountPath"))->enabled) {
        while (pt) {
			if(fanotify_mark(onas_fan_fd, FAN_MARK_ADD | FAN_MARK_MOUNT, (*ctx)->fan_mask, (*ctx)->fan_fd, pt->strarg) != 0) {
				logg("!ClamFanotif: can't include mountpoint '%s'\n", pt->strarg);
				return CL_EARG;
            } else
				logg("*ClamFanotif: recursively watching the mount point '%s'\n", pt->strarg);
            pt = (struct optstruct *)pt->nextarg;
        }

	} else if (!optget((*ctx)->clamdopts, "OnAccessDisableDDD")->enabled) {
		(*ctx)->ddd_enabled = 1;
    } else {
		if((pt = optget((*ctx)->clamdopts, "OnAccessIncludePath"))->enabled) {
            while (pt) {
				if(fanotify_mark(onas_fan_fd, FAN_MARK_ADD, (*ctx)->fan_mask, (*ctx)->fan_fd, pt->strarg) != 0) {
					logg("!ClamFanotif: can't include path '%s'\n", pt->strarg);
					return CL_EARG;
                } else
					logg("*ClamFanotif: watching directory '%s' (non-recursively)\n", pt->strarg);
                pt = (struct optstruct *)pt->nextarg;
            }
        } else {
			logg("!ClamFanotif: please specify at least one path with OnAccessIncludePath\n");
			return CL_EARG;
        }
    }

    /* Load other options. */
	(*ctx)->sizelimit = optget((*ctx)->clamdopts, "OnAccessMaxFileSize")->numarg;
	if((*ctx)->sizelimit)
		logg("*ClamFanotif: max file size limited to %lu bytes\n", (*ctx)->sizelimit);
    else
		logg("*ClamFanotif: file size limit disabled\n");

	extinfo = optget((*ctx)->clamdopts, "ExtendedDetectionInfo")->enabled;

	return CL_SUCCESS;
}

int onas_fan_eloop(struct onas_context **ctx) {
	int ret = 0;
        int err_cnt = 0;
	sigset_t sigset;
	struct sigaction act;
	short int scan;
	STATBUF sb;
	fd_set rfds;
	char buf[4096];
	ssize_t bread;
	struct fanotify_event_metadata *fmd;
	char fname[1024];
	int len, check, fres;
	char err[128];

	/* ignore all signals except SIGUSR1 */
	sigfillset(&sigset);
	sigdelset(&sigset, SIGUSR1);
	/* The behavior of a process is undefined after it ignores a
	 * SIGFPE, SIGILL, SIGSEGV, or SIGBUS signal */
	sigdelset(&sigset, SIGFPE);
	sigdelset(&sigset, SIGILL);
	sigdelset(&sigset, SIGSEGV);
	sigdelset(&sigset, SIGINT);
#ifdef SIGBUS
	sigdelset(&sigset, SIGBUS);
#endif
	pthread_sigmask(SIG_SETMASK, &sigset, NULL);
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = onas_fan_exit;
	sigfillset(&(act.sa_mask));
	sigaction(SIGUSR1, &act, NULL);
	sigaction(SIGSEGV, &act, NULL);
	sigaction(SIGINT, &act, NULL);

    FD_ZERO(&rfds);
	FD_SET((*ctx)->fan_fd, &rfds);
    do {
		ret = select((*ctx)->fan_fd + 1, &rfds, NULL, NULL, NULL);
	} while((ret == -1 && errno == EINTR));

    time_t start = time(NULL) - 30;
	while(((bread = read((*ctx)->fan_fd, buf, sizeof(buf))) > 0) || (errno == EOVERFLOW || errno == EMFILE || errno == EACCES)) {
		switch(errno) {
			case EOVERFLOW:
            if (time(NULL) - start >= 30) {
					logg("*ClamFanotif: internal error (failed to read data) ... %s\n", strerror(errno));
					logg("*ClamFanotif: file too large for fanotify ... recovering and continuing scans...\n");
                start = time(NULL);
            }

            errno = 0;
            continue;
			case EACCES:
				logg("*ClamFanotif: internal error (failed to read data) ... %s\n", strerror(errno));
				logg("*ClamFanotif: check your SELinux audit logs and consider adding an exception \
						... recovering and continuing scans...\n");

				errno = 0;
				continue;
			case EMFILE:
				logg("*ClamFanotif: internal error (failed to read data) ... %s\n", strerror(errno));
                                logg("*ClamFanotif: waiting for consumer thread to catch up then retrying ...\n");
                                sleep(3);

				errno = 0;
                                continue;
			default:
			break;
                }

        fmd = (struct fanotify_event_metadata *)buf;
        while (FAN_EVENT_OK(fmd, bread)) {
            scan = 1;
            if (fmd->fd >= 0) {
                sprintf(fname, "/proc/self/fd/%d", fmd->fd);
                                errno = 0;
                len = readlink(fname, fname, sizeof(fname) - 1);
                if (len == -1) {
                    close(fmd->fd);
					logg("!ClamFanotif: internal error (readlink() failed), %d, %s\n", fmd->fd, strerror(errno));
                                        if (errno == EBADF) {
                                            logg("ClamWorker: fd already closed ... recovering ...\n");
                                            continue;
                                        } else {
					return 2;
                }
				}
				fname[len] = '\0';

				if((check = onas_fan_checkowner(fmd->pid, (*ctx)->clamdopts))) {
                    scan = 0;
                    if (check != CHK_SELF) {
							logg("*ClamFanotif: %s skipped (excluded UID)\n", fname);
                }
            }

                                if (scan) {
					struct onas_scan_event *event_data;

					event_data = cli_calloc(1, sizeof(struct onas_scan_event));
                                        if (NULL == event_data) {
					    logg("!ClamFanotif: could not allocate memory for event data struct\n");
                                            return 2;
                                        }

                                        /* general mapping */
                                        onas_map_context_info_to_event_data(*ctx, &event_data);
					scan ? event_data->bool_opts |= ONAS_SCTH_B_SCAN : scan;

                                        /* fanotify specific stuffs */
					event_data->bool_opts |= ONAS_SCTH_B_FANOTIFY;
                                        event_data->fmd = cli_malloc(sizeof(struct fanotify_event_metadata));
                                        if (NULL == event_data->fmd) {
					    logg("!ClamFanotif: could not allocate memory for event data struct fmd\n");
                                            return 2;
                                        }
					memcpy(event_data->fmd, fmd, sizeof(struct fanotify_event_metadata));
                                        event_data->pathname = cli_strdup(fname);


					logg("*ClamFanotif: attempting to feed consumer queue\n");
					/* feed consumer queue */
					if (CL_SUCCESS != onas_queue_event(event_data)) {
                close(fmd->fd);
						logg("!ClamFanotif: error occurred while feeding consumer queue ... \n");
                                                if ((*ctx)->retry_on_error) {
                                                    err_cnt++;
                                                    if (err_cnt < (*ctx)->retry_attempts) {
                                                        logg("ClamFanotif: ... recovering ...\n");
                                                        continue;
                                                    }
                                                }
						return 2;
					}
                                } else {
                                    if (fmd->mask & FAN_ALL_PERM_EVENTS) {
                                        struct fanotify_response res;

                                        res.fd = fmd->fd;
                                        res.response = FAN_ALLOW;

                                        if (-1 == write((*ctx)->fan_fd, &res, sizeof(res))) {
						logg("!ClamFanotif: error occurred while excluding event\n");
                                                return 2;
                                        }
					}

                                        if (-1 == close(fmd->fd)) {
						logg("!ClamFanotif: error occurred while closing metadata fd, %d\n", fmd->fd);
                                                if (errno == EBADF) {
                                                    logg("ClamFanotif: fd already closed ... recovering ...\n");
                                                } else {
                                                return 2;
                                        }
                                    }
                                }
        }
        fmd = FAN_EVENT_NEXT(fmd, bread);
    }
    do {
				ret = select((*ctx)->fan_fd + 1, &rfds, NULL, NULL, NULL);
		} while((ret == -1 && errno == EINTR));
}

		if(bread < 0) {
		logg("!ClamFanotif: internal error (failed to read data) ... %s\n", strerror(errno));
                        return 2;
                }


	return ret;
}
#endif
