/*
 *   stunnel       Universal SSL tunnel
 *   Copyright (c) 1998-2001 Michal Trojnara <Michal.Trojnara@mirt.net>
 *                 All Rights Reserved
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "common.h"

extern server_options options;

FILE *outfile=NULL; /* Logging to file disabled by default */

#ifdef USE_WIN32

/* HANDLE evt=NULL; */

void log_open() { /* Win32 version */
    if(options.output_file)
        outfile=fopen(options.output_file, "a");
    if(outfile)
        return; /* It was possible o open a log file */
    /* TODO: Register NT EventLog source here */
    /* evt=RegisterEventSource(NULL, "stunnel"); */
    if(options.output_file)
        log(LOG_ERR, "Unable to open output file: %s", options.output_file);
}

void log_close() {
    if(outfile)
        fclose(outfile);
}

#else /* USE_WIN32 */

void log_open() { /* Unix version */
    if(options.output_file)
        outfile=fopen(options.output_file, "a");
    if(outfile)
        return; /* It was possible o open a log file */
    if(!options.foreground) {
#ifdef __ultrix__
        openlog("stunnel", LOG_PID);
#else
        openlog("stunnel", LOG_CONS | LOG_NDELAY | LOG_PID, options.facility);
#endif /* __ultrix__ */
    }
    if(options.output_file)
        log(LOG_ERR, "Unable to open output file: %s", options.output_file);
}

void log_close() {
    if(outfile) {
        fclose(outfile);
        return;
    }
    if(!options.foreground)
        closelog();
}

#endif /* USE_WIN32 */

void log(int level, char *format, ...) {
    va_list arglist;
    char text[256];
    FILE *out;
    time_t gmt;
    struct tm *timeptr;

    if(level>options.debug_level)
        return;
    va_start(arglist, format);
#ifdef HAVE_VSNPRINTF
    vsnprintf(text, 256, format, arglist);
#else
    vsprintf(text, format, arglist);
#endif
    va_end(arglist);
#ifndef USE_WIN32
    if(!outfile && !options.foreground) {
        syslog(level, "%s", text);
        return;
    }
#endif
    out=outfile?outfile:stderr;
    time(&gmt);
    timeptr=localtime(&gmt);
    fprintf(out, "%04d.%02d.%02d %02d:%02d:%02d LOG%d[%lu:%lu]: %s\n",
        timeptr->tm_year+1900, timeptr->tm_mon+1, timeptr->tm_mday,
        timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec,
        level, process_id(), thread_id(), text);
    fflush(out);
}

/* End of log.c */
