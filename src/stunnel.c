/*
 *   stunnel       Universal SSL tunnel
 *   Copyright (c) 1998-2005 Michal Trojnara <Michal.Trojnara@mirt.net>
 *                 All Rights Reserved
 *
 *   Version:      4.08             (stunnel.c)
 *   Date:         2005.02.27
 *
 *   Author:       Michal Trojnara  <Michal.Trojnara@mirt.net>
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
 *
 *   In addition, as a special exception, Michal Trojnara gives
 *   permission to link the code of this program with the OpenSSL
 *   library (or with modified versions of OpenSSL that use the same
 *   license as OpenSSL), and distribute linked combinations including
 *   the two.  You must obey the GNU General Public License in all
 *   respects for all of the code used other than OpenSSL.  If you modify
 *   this file, you may extend this exception to your version of the
 *   file, but you are not obligated to do so.  If you do not wish to
 *   do so, delete this exception statement from your version.
 */

#include "common.h"
#include "prototypes.h"

    /* Prototypes */
static void daemon_loop(void);
static void accept_connection(LOCAL_OPTIONS *);
static void get_limits(void); /* setup global max_clients and max_fds */
#if !defined (USE_WIN32) && !defined (__vms)
static void drop_privileges(void);
static void daemonize(void);
static void create_pid(void);
static void delete_pid(void);
#endif
static void setnonblock(int, unsigned long);

    /* Error/exceptions handling functions */
#ifndef USE_WIN32
static void signal_handler(int);
#endif

int num_clients=0; /* Current number of clients */

    /* Functions */

#ifndef USE_WIN32
int main(int argc, char* argv[]) { /* execution begins here 8-) */

    main_initialize(argc>1 ? argv[1] : NULL, argc>2 ? argv[2] : NULL);

    signal(SIGPIPE, SIG_IGN); /* avoid 'broken pipe' signal */
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGHUP, signal_handler);
    /* signal(SIGSEGV, signal_handler); */

    main_execute();

    return 0; /* success */
}
#endif

void main_initialize(char *arg1, char *arg2) {
    struct stat st; /* buffer for stat */

    sthreads_init(); /* initialize critical sections & SSL callbacks */
    parse_config(arg1, arg2);
    log_open();
    s_log(LOG_NOTICE, "%s", stunnel_info());

    /* check if certificate exists */
    if(!options.key) /* key file not specified */
        options.key=options.cert;
    if(options.option.cert) {
        if(stat(options.key, &st)) {
            ioerror(options.key);
            exit(1);
        }
#ifndef USE_WIN32
        if(st.st_mode & 7)
            s_log(LOG_WARNING, "Wrong permissions on %s", options.key);
#endif /* defined USE_WIN32 */
    }
}

void main_execute(void) {
    ssl_init(); /* initialize SSL library */
    context_init(); /* initialize global SSL context */
    /* check if started from inetd */
    if(local_options.next) { /* there are service sections -> daemon mode */
        daemon_loop();
    } else { /* inetd mode */
#if !defined (USE_WIN32) && !defined (__vms)
        max_fds=FD_SETSIZE; /* just in case */
        drop_privileges();
#endif
        num_clients=1;
        client(alloc_client_session(&local_options, 0, 1));
    }
    /* close SSL */
    context_free(); /* free global SSL context */
    log_close();
}

static void daemon_loop(void) {
    SOCKADDR_UNION addr;
    s_poll_set fds;
    LOCAL_OPTIONS *opt;

    get_limits();
    s_poll_zero(&fds);
#ifndef USE_WIN32
    s_poll_add(&fds, signal_pipe_init(), 1, 0);
#endif

    if(!local_options.next) {
        s_log(LOG_ERR, "No connections defined in config file");
        exit(1);
    }

    num_clients=0;

    /* bind local ports */
    for(opt=local_options.next; opt; opt=opt->next) {
        if(!opt->option.accept) /* no need to bind this service */
            continue;
        memcpy(&addr, &opt->local_addr.addr[0], sizeof(SOCKADDR_UNION));
        if((opt->fd=socket(addr.sa.sa_family, SOCK_STREAM, 0))<0) {
            sockerror("local socket");
            exit(1);
        }
        if(alloc_fd(opt->fd))
            exit(1);
        if(set_socket_options(opt->fd, 0)<0)
            exit(1);
        s_ntop(opt->local_address, &addr);
        if(bind(opt->fd, &addr.sa, addr_len(addr))) {
            s_log(LOG_ERR, "Error binding %s to %s",
                opt->servname, opt->local_address);
            sockerror("bind");
            exit(1);
        }
        s_log(LOG_DEBUG, "%s bound to %s", opt->servname, opt->local_address);
        if(listen(opt->fd, 5)) {
            sockerror("listen");
            exit(1);
        }
#ifdef FD_CLOEXEC
        fcntl(opt->fd, F_SETFD, FD_CLOEXEC); /* close socket in child execvp */
#endif
        s_poll_add(&fds, opt->fd, 1, 0);
    }

#if !defined (USE_WIN32) && !defined (__vms)
    if(!(options.option.foreground))
        daemonize();
    drop_privileges();
    create_pid();
#endif /* !defined USE_WIN32 && !defined (__vms) */

    /* create exec+connect services */
    for(opt=local_options.next; opt; opt=opt->next) {
        if(opt->option.accept) /* skip ordinary (accepting) services */
            continue;
        enter_critical_section(CRIT_CLIENTS); /* for multi-cpu machines */
        num_clients++;
        leave_critical_section(CRIT_CLIENTS);
        create_client(-1, -1, alloc_client_session(opt, -1, -1), client);
    }

    while(1) {
        if(s_poll_wait(&fds, -1)<0) /* non-critical error */
            log_error(LOG_INFO, get_last_socket_error(),
                "daemon_loop: s_poll_wait");
        else 
            for(opt=local_options.next; opt; opt=opt->next)
                if(s_poll_canread(&fds, opt->fd))
                    accept_connection(opt);
    }
    s_log(LOG_ERR, "INTERNAL ERROR: End of infinite loop 8-)");
}

static void accept_connection(LOCAL_OPTIONS *opt) {
    SOCKADDR_UNION addr;
    char from_address[IPLEN];
    int s, addrlen=sizeof(SOCKADDR_UNION);

    while((s=accept(opt->fd, &addr.sa, &addrlen))<0) {
        switch(get_last_socket_error()) {
            case EINTR:
                break; /* retry */
            case EMFILE:
            case ENFILE:
#ifdef ENOBUFS
            case ENOBUFS:
#endif
            case ENOMEM:
                sleep(1); /* temporarily out of resources - short delay */
            default:
                sockerror("accept");
                return; /* error */
        }
    }
    s_ntop(from_address, &addr);
    s_log(LOG_DEBUG, "%s accepted FD=%d from %s",
        opt->servname, s, from_address);
    if(max_clients && num_clients>=max_clients) {
        s_log(LOG_WARNING, "Connection rejected: too many clients (>=%d)",
            max_clients);
        closesocket(s);
        return;
    }
    if(alloc_fd(s))
        return;
#ifdef FD_CLOEXEC
    fcntl(s, F_SETFD, FD_CLOEXEC); /* close socket in child execvp */
#endif
    if(create_client(opt->fd, s, alloc_client_session(opt, s, s), client)) {
        s_log(LOG_ERR, "Connection rejected: create_client failed");
        closesocket(s);
        return;
    }
    enter_critical_section(CRIT_CLIENTS); /* for multi-cpu machines */
    num_clients++;
    leave_critical_section(CRIT_CLIENTS);
}

static void get_limits(void) {
#ifdef USE_WIN32
    max_clients=0;
    s_log(LOG_NOTICE, "No limit detected for the number of clients");
#else
    max_fds=0; /* unlimited */

#if defined HAVE_SYSCONF
    max_fds=sysconf(_SC_OPEN_MAX);
    if(max_fds<0)
        ioerror("sysconf");
#elif defined HAVE_GETRLIMIT
    struct rlimit rlim;
    if(getrlimit(RLIMIT_NOFILE, &rlim)<0)
        ioerror("getrlimit");
    else
        max_fds=rlim.rlim_cur;
    if(max_fds==RLIM_INFINITY)
        max_fds=0; /* RLIM_INFINITY should be equal to zero, anyway */
#endif
    s_log(LOG_INFO, "file ulimit = %d%s (can be changed with 'ulimit -n')",
        max_fds, max_fds ? "" : " (unlimited)");
#ifdef HAVE_POLL
    s_log(LOG_INFO, "poll() used - no FD_SETSIZE limit for file descriptors");
#else
    s_log(LOG_INFO,
        "FD_SETSIZE = %d (some systems allow to increase this value)",
        FD_SETSIZE);
    if(!max_fds || max_fds>FD_SETSIZE)
        max_fds=FD_SETSIZE;
#endif
    if(max_fds && max_fds<16) /* stunnel needs at least 16 file desriptors */
        max_fds=16;
    if(max_fds) {
        max_clients=max_fds>=256 ? max_fds*125/256 : (max_fds-6)/2;
        s_log(LOG_NOTICE, "%d clients allowed", max_clients);
    } else {
        max_clients=0;
        s_log(LOG_NOTICE, "No limit detected for the number of clients");
    }
#endif
}

#if !defined (USE_WIN32) && !defined (__vms)
    /* chroot and set process user and group(s) id */
static void drop_privileges(void) {
    int uid=0, gid=0;
    struct group *gr;
#ifdef HAVE_SETGROUPS
    gid_t gr_list[1];
#endif
    struct passwd *pw;

    /* Get the integer values */
    if(options.setgid_group) {
        gr=getgrnam(options.setgid_group);
        if(gr)
            gid=gr->gr_gid;
        else if(atoi(options.setgid_group)) /* numerical? */
            gid=atoi(options.setgid_group);
        else {
            s_log(LOG_ERR, "Failed to get GID for group %s",
                options.setgid_group);
            exit(1);
        }
    }
    if(options.setuid_user) {
        pw=getpwnam(options.setuid_user);
        if(pw)
            uid=pw->pw_uid;
        else if(atoi(options.setuid_user)) /* numerical? */
            uid=atoi(options.setuid_user);
        else {
            s_log(LOG_ERR, "Failed to get UID for user %s",
                options.setuid_user);
            exit(1);
        }
    }

#ifdef HAVE_CHROOT
    /* chroot */
    if(options.chroot_dir) {
        if(chroot(options.chroot_dir)) {
            sockerror("chroot");
            exit(1);
        }
        if(chdir("/")) {
            sockerror("chdir");
            exit(1);
        }
    }
#endif /* HAVE_CHROOT */

    /* Set uid and gid */
    if(gid) {
        if(setgid(gid)) {
            sockerror("setgid");
            exit(1);
        }
#ifdef HAVE_SETGROUPS
        gr_list[0]=gid;
        if(setgroups(1, gr_list)) {
            sockerror("setgroups");
            exit(1);
        }
#endif
    }
    if(uid) {
        if(setuid(uid)) {
            sockerror("setuid");
            exit(1);
        }
    }
}

static void daemonize(void) { /* go to background */
#if defined(HAVE_DAEMON) && !defined(__BEOS__)
    if(daemon(0,0)==-1) {
        ioerror("daemon");
        exit(1);
    }
#else
    chdir("/");
    switch(fork()) {
    case -1:    /* fork failed */
        ioerror("fork");
        exit(1);
    case 0:     /* child */
        break;
    default:    /* parent */
        exit(0);
    }
    close(0);
    close(1);
    close(2);
#endif
#ifdef HAVE_SETSID
    setsid(); /* Ignore the error */
#endif
}

static void create_pid(void) {
    int pf;
    char pid[STRLEN];

    if(!options.pidfile) {
        s_log(LOG_DEBUG, "No pid file being created");
        return;
    }
    if(options.pidfile[0]!='/') {
        s_log(LOG_ERR, "Pid file (%s) must be full path name", options.pidfile);
        /* Why?  Because we don't want to confuse by
           allowing '.', which would be '/' after
           daemonizing) */
        exit(1);
    }
    options.dpid=(unsigned long)getpid();

    /* silently remove old pid file */
    unlink(options.pidfile);
    if((pf=open(options.pidfile, O_WRONLY|O_CREAT|O_TRUNC|O_EXCL,0644))==-1) {
        s_log(LOG_ERR, "Cannot create pid file %s", options.pidfile);
        ioerror("create");
        exit(1);
    }
    sprintf(pid, "%lu\n", options.dpid);
    write(pf, pid, strlen(pid));
    close(pf);
    s_log(LOG_DEBUG, "Created pid file %s", options.pidfile);
    atexit(delete_pid);
}

static void delete_pid(void) {
    s_log(LOG_DEBUG, "removing pid file %s", options.pidfile);
    if((unsigned long)getpid()!=options.dpid)
        return; /* current process is not main daemon process */
    if(unlink(options.pidfile)<0)
        ioerror(options.pidfile); /* not critical */
}
#endif /* defined USE_WIN32 */

int set_socket_options(int s, int type) {
    SOCK_OPT *ptr;
    extern SOCK_OPT sock_opts[];
    static char *type_str[3]={"accept", "local", "remote"};
    int opt_size;

    for(ptr=sock_opts;ptr->opt_str;ptr++) {
        if(!ptr->opt_val[type])
            continue; /* default */
        switch(ptr->opt_type) {
        case TYPE_LINGER:
            opt_size=sizeof(struct linger); break;
        case TYPE_TIMEVAL:
            opt_size=sizeof(struct timeval); break;
        case TYPE_STRING:
            opt_size=strlen(ptr->opt_val[type]->c_val)+1; break;
        default:
            opt_size=sizeof(int); break;
        }
        if(setsockopt(s, ptr->opt_level, ptr->opt_name,
                (void *)ptr->opt_val[type], opt_size)) {
            sockerror(ptr->opt_str);
            return -1; /* FAILED */
        } else {
            s_log(LOG_DEBUG, "%s option set on %s socket",
                ptr->opt_str, type_str[type]);
        }
    }
    return 0; /* OK */
}

void ioerror(const char *txt) { /* input/output error handler */
    log_error(LOG_ERR, get_last_error(), txt);
}

void sockerror(const char *txt) { /* socket error handler */
    log_error(LOG_ERR, get_last_socket_error(), txt);
}

void log_error(int level, int error, const char *txt) { /* generic error logger */
    s_log(level, "%s: %s (%d)", txt, my_strerror(error), error);
}

char *my_strerror(int errnum) {
    switch(errnum) {
#ifdef USE_WIN32
    case 10004:
        return "Interrupted system call (WSAEINTR)";
    case 10009:
        return "Bad file number (WSAEBADF)";
    case 10013:
        return "Permission denied (WSAEACCES)";
    case 10014:
        return "Bad address (WSAEFAULT)";
    case 10022:
        return "Invalid argument (WSAEINVAL)";
    case 10024:
        return "Too many open files (WSAEMFILE)";
    case 10035:
        return "Operation would block (WSAEWOULDBLOCK)";
    case 10036:
        return "Operation now in progress (WSAEINPROGRESS)";
    case 10037:
        return "Operation already in progress (WSAEALREADY)";
    case 10038:
        return "Socket operation on non-socket (WSAENOTSOCK)";
    case 10039:
        return "Destination address required (WSAEDESTADDRREQ)";
    case 10040:
        return "Message too long (WSAEMSGSIZE)";
    case 10041:
        return "Protocol wrong type for socket (WSAEPROTOTYPE)";
    case 10042:
        return "Bad protocol option (WSAENOPROTOOPT)";
    case 10043:
        return "Protocol not supported (WSAEPROTONOSUPPORT)";
    case 10044:
        return "Socket type not supported (WSAESOCKTNOSUPPORT)";
    case 10045:
        return "Operation not supported on socket (WSAEOPNOTSUPP)";
    case 10046:
        return "Protocol family not supported (WSAEPFNOSUPPORT)";
    case 10047:
        return "Address family not supported by protocol family (WSAEAFNOSUPPORT)";
    case 10048:
        return "Address already in use (WSAEADDRINUSE)";
    case 10049:
        return "Can't assign requested address (WSAEADDRNOTAVAIL)";
    case 10050:
        return "Network is down (WSAENETDOWN)";
    case 10051:
        return "Network is unreachable (WSAENETUNREACH)";
    case 10052:
        return "Net dropped connection or reset (WSAENETRESET)";
    case 10053:
        return "Software caused connection abort (WSAECONNABORTED)";
    case 10054:
        return "Connection reset by peer (WSAECONNRESET)";
    case 10055:
        return "No buffer space available (WSAENOBUFS)";
    case 10056:
        return "Socket is already connected (WSAEISCONN)";
    case 10057:
        return "Socket is not connected (WSAENOTCONN)";
    case 10058:
        return "Can't send after socket shutdown (WSAESHUTDOWN)";
    case 10059:
        return "Too many references, can't splice (WSAETOOMANYREFS)";
    case 10060:
        return "Connection timed out (WSAETIMEDOUT)";
    case 10061:
        return "Connection refused (WSAECONNREFUSED)";
    case 10062:
        return "Too many levels of symbolic links (WSAELOOP)";
    case 10063:
        return "File name too long (WSAENAMETOOLONG)";
    case 10064:
        return "Host is down (WSAEHOSTDOWN)";
    case 10065:
        return "No Route to Host (WSAEHOSTUNREACH)";
    case 10066:
        return "Directory not empty (WSAENOTEMPTY)";
    case 10067:
        return "Too many processes (WSAEPROCLIM)";
    case 10068:
        return "Too many users (WSAEUSERS)";
    case 10069:
        return "Disc Quota Exceeded (WSAEDQUOT)";
    case 10070:
        return "Stale NFS file handle (WSAESTALE)";
    case 10091:
        return "Network SubSystem is unavailable (WSASYSNOTREADY)";
    case 10092:
        return "WINSOCK DLL Version out of range (WSAVERNOTSUPPORTED)";
    case 10093:
        return "Successful WSASTARTUP not yet performed (WSANOTINITIALISED)";
    case 10071:
        return "Too many levels of remote in path (WSAEREMOTE)";
    case 11001:
        return "Host not found (WSAHOST_NOT_FOUND)";
    case 11002:
        return "Non-Authoritative Host not found (WSATRY_AGAIN)";
    case 11003:
        return "Non-Recoverable errors: FORMERR, REFUSED, NOTIMP (WSANO_RECOVERY)";
    case 11004:
        return "Valid name, no data record of requested type (WSANO_DATA)";
#if 0
    case 11004: /* typically, only WSANO_DATA is reported */
        return "No address, look for MX record (WSANO_ADDRESS)";
#endif
#endif /* defined USE_WIN32 */
    default:
        return strerror(errnum);
    }
}

#ifndef USE_WIN32

static void signal_handler(int sig) { /* signal handler */
    s_log(sig==SIGTERM ? LOG_NOTICE : LOG_ERR,
        "Received signal %d; terminating", sig);
    exit(3);
}

#endif /* !defined USE_WIN32 */

char *stunnel_info(void) {
    static char retval[STRLEN];

    safecopy(retval, "stunnel " VERSION " on " HOST);
#ifdef USE_PTHREAD
    safeconcat(retval, " PTHREAD");
#endif
#ifdef USE_WIN32
    safeconcat(retval, " WIN32");
#endif
#ifdef USE_FORK
    safeconcat(retval, " FORK");
#endif
#ifdef HAVE_POLL
    safeconcat(retval, "+POLL");
#endif
#ifdef USE_WIN32
    if(s_getaddrinfo)
        safeconcat(retval, "+IPv6");
    else
        safeconcat(retval, "+IPv4");
#else /* defined(USE_WIN32) */
#if defined(USE_IPv6)
    safeconcat(retval, "+IPv6");
#else /* defined(USE_IPv6) */
    safeconcat(retval, "+IPv4");
#endif /* defined(USE_IPv6) */
#endif /* defined(USE_WIN32) */
#ifdef USE_LIBWRAP
    safeconcat(retval, "+LIBWRAP");
#endif
    safeconcat(retval, " with ");
    safeconcat(retval, SSLeay_version(SSLEAY_VERSION));
    return retval;
}

int alloc_fd(int sock) {
#ifndef USE_WIN32
    if(!max_fds || sock>=max_fds) {
        s_log(LOG_ERR,
            "File descriptor out of range (%d>=%d)", sock, max_fds);
        closesocket(sock);
        return -1;
    }
#endif
    setnonblock(sock, 1);
    return 0;
}

/* Try to use non-POSIX O_NDELAY on obsolete BSD systems */
#if !defined O_NONBLOCK && defined O_NDELAY
#define O_NONBLOCK O_NDELAY
#endif

static void setnonblock(int sock, unsigned long l) {
#if defined F_GETFL && defined F_SETFL && defined O_NONBLOCK
    int retval, flags;
    do {
        flags=fcntl(sock, F_GETFL, 0);
    }while(flags<0 && get_last_socket_error()==EINTR);
    flags=l ? flags|O_NONBLOCK : flags&(~O_NONBLOCK);
    do {
        retval=fcntl(sock, F_SETFL, flags);
    }while(retval<0 && get_last_socket_error()==EINTR);
    if(retval<0)
#else
    if(ioctlsocket(sock, FIONBIO, &l)<0)
#endif
        sockerror("nonblocking"); /* non-critical */
    else
        s_log(LOG_DEBUG, "FD %d in %sblocking mode", sock,
            l ? "non-" : "");
}

/* End of stunnel.c */
