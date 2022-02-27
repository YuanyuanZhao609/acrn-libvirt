#include <config.h>
#include <fcntl.h>
#include <sys/sysinfo.h>
#include <sys/ioctl.h>
#include "datatypes.h"
#include "virfile.h"
#include "viralloc.h"
#include "virutil.h"
#include "vircommand.h"
#include "virthread.h"
#include "virstring.h"
#include "domain_event.h"
#include "virjson.h"
#include "acrn_manager.h"
#include "virtime.h"
#include "virerror.h"
#include "virlog.h"
#include "acrn_domain.h"

#define VIR_FROM_THIS VIR_FROM_ACRN
#define ACRN_DEFAULT_MONITOR_WAIT 30

#define LINE_ENDING "\n"

VIR_LOG_INIT("acrn.acrn_manager");

struct _acrnManager {
    int fd;
    int watch;
    virDomainObjPtr vm;
    virMutex lock;

    acrnManagerMessagePtr msg;
    /* Buffer incoming data ready for acrn lifecycle manager
     * code to process & find message boundaries */
    char buffer[1024];

    acrnManagerStopCallback stop;
    int shutdown_reason;
};

static int
acrnManagerIOWriteWithFD(acrnManagerPtr mon,
                         const char *data,
                         size_t len,
                         int fd)
{
    struct msghdr msg;
    struct iovec iov[1];
    int ret;
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;

    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));

    iov[0].iov_base = (void *)data;
    iov[0].iov_len = len;

    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    cmsg = CMSG_FIRSTHDR(&msg);
    /* Some static analyzers, like clang 2.6-0.6.pre2, fail to see
       that our use of CMSG_FIRSTHDR will not return NULL.  */
    sa_assert(cmsg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    do {
        ret = sendmsg(mon->fd, &msg, 0);
    } while (ret < 0 && errno == EINTR);

    return ret;
}

static int
acrnManagerIORead(acrnManagerPtr mon)
{
    int ret = 0;
    int got;

    memset(mon->buffer, 0x0, 1024);
    got = read(mon->fd,
                mon->buffer,
                1023);
    if (got < 0) {
        if (errno == EAGAIN)
        virReportSystemError(errno, "%s",
                                _("Unable to read from monitor"));
        ret = -1;
    }

    ret += got;
    mon->buffer[got] = '\0';

    VIR_DEBUG("Now read %d bytes of data: %s.", got, mon->buffer);


    return ret;
}

static int
acrnManagerIOProcess(acrnManagerPtr mon)
{
    return 0;
}


static int
acrnManagerCommand(acrnManagerPtr mon,
                 const char *cmd,
                 int seconds)
{
    int ret = -1;
    char *txBuffer;
    int txLength;
    acrnManagerMessage msg;
    virDomainObjPtr vm = mon->vm;

    msg.rxObject = NULL;

    VIR_DEBUG("acrnManagerCommand: $s", cmd);
    txBuffer = g_strdup_printf("%s:%s", cmd, vm->def->name);
    txLength = strlen(txBuffer);

    mon->msg = &msg;

    VIR_DEBUG("Send command '%s' for write, seconds = %d", txBuffer, seconds);

    ret = acrnManagerIOWriteWithFD(mon, txBuffer, txLength, mon->fd);

    VIR_DEBUG("Receive command reply ret=%d", ret);

 cleanup:
    VIR_FREE(txBuffer);

    return 0;
}

int
acrnManagerSystemPowerdown(acrnManagerPtr mon)
{
    int ret;
    const char *cmd = "user_vm_shutdown";

    if (!mon)
        return -1;
    VIR_DEBUG("acrnManagerSystemPowerdown: send shutdown command");

    ret = acrnManagerCommand(mon, cmd, 60);

    return ret;
}

static void
acrnManagerUpdateWatch(acrnManagerPtr mon)
{
    int events =
        VIR_EVENT_HANDLE_HANGUP |
        VIR_EVENT_HANDLE_ERROR;

    if (!mon->watch)
        return;

    events |= VIR_EVENT_HANDLE_READABLE;
    virEventUpdateHandle(mon->watch, events);
}

static void
acrnManagerIO(int watch, int fd, int events, void *opaque)
{
    acrnManagerPtr mon = opaque;
    bool error = false;
    bool eof = false;
    bool hangup = false;

    virObjectRef(mon);
    /* lock access to the monitor and protect fd */
    virObjectLock(mon);

    if (mon->fd == -1 || mon->watch == 0) {
        virObjectUnlock(mon);
        virObjectUnref(mon);
        return;
    }

    VIR_DEBUG("Manager %p I/O on watch %d fd %d events %d", mon, watch, fd, events);
    if (mon->fd != fd || mon->watch != watch) {
        if (events & (VIR_EVENT_HANDLE_HANGUP | VIR_EVENT_HANDLE_ERROR))
            eof = true;
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("event from unexpected fd %d!=%d / watch %d!=%d"),
                       mon->fd, fd, mon->watch, watch);
        error = true;
    } else {
        if (!error && (events & VIR_EVENT_HANDLE_READABLE)) {
            int got = acrnManagerIORead(mon);
            events &= ~VIR_EVENT_HANDLE_READABLE;
            if (got < 0) {
                error = true;
                if (errno == ECONNRESET)
                    hangup = true;
            } else if (got == 0) {
                eof = true;
            } else {
                /* Ignore hangup/error events if we read some data, to
                 * give time for that data to be consumed */
                events = 0;

                if (acrnManagerIOProcess(mon) < 0)
                    error = true;
            }
        }

        if (events & VIR_EVENT_HANDLE_HANGUP) {
            hangup = true;
            if (!error) {
                VIR_DEBUG("End of file from acrn manager");
                eof = true;
                events &= ~VIR_EVENT_HANDLE_HANGUP;
            }
        }

        if (!error && !eof &&
            events & VIR_EVENT_HANDLE_ERROR) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                            _("Invalid file descriptor while waiting for manager"));
            eof = true;
            events &= ~VIR_EVENT_HANDLE_ERROR;
        }

    }

    acrnManagerUpdateWatch(mon);

    if (eof) {
        VIR_DEBUG("acrnManagerIO: EOF.");
        mon->shutdown_reason = VIR_DOMAIN_SHUTOFF_SHUTDOWN;
        virObjectUnlock(mon);
        acrnManagerUnregister(mon);
        virObjectUnref(mon);
    } else if (error) {
        VIR_DEBUG("acrnManagerIO: error.");
        virObjectUnlock(mon);
        acrnManagerUnregister(mon);
        virObjectUnref(mon);
    } else {
        mon->shutdown_reason = VIR_DOMAIN_SHUTOFF_UNKNOWN;
        virObjectUnlock(mon);
        acrnManagerUnregister(mon);
        virObjectUnref(mon);
    }
}

bool
acrnManagerRegister(acrnManagerPtr mon)
{
    virObjectRef(mon);
    if ((mon->watch = virEventAddHandle(mon->fd,
                                        VIR_EVENT_HANDLE_HANGUP |
                                        VIR_EVENT_HANDLE_ERROR |
                                        VIR_EVENT_HANDLE_READABLE,
                                        acrnManagerIO,
                                        mon,
                                        virObjectFreeCallback)) < 0) {
        virObjectUnref(mon);
        return false;
    }

    return true;
}

void
acrnManagerUnregister(acrnManagerPtr mon)
{
    if (mon->watch) {
        virEventRemoveHandle(mon->watch);
        mon->watch = 0;
    }
}

int
acrnManagerGetReason(acrnManagerPtr mon)
{
    return mon->shutdown_reason;
}

static int
acrnManagerOpenUnix(const char *monitor)
{
    struct sockaddr_un addr;
    int monfd;
    int ret = -1;
    virTimeBackOffVar timebackoff;

    if (monitor == NULL) {
        VIR_DEBUG("Socket path is NULL");
        return -1;
    }
    if ((monfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(errno,
                             "%s", _("failed to create socket"));
        return -1;
    }
    VIR_DEBUG("acrnManagerOpenUnix:create sock");
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, monitor) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Monitor path %s too big for destination"), monitor);
        goto error;
    }
    if (virTimeBackOffStart(&timebackoff, 1, ACRN_DEFAULT_MONITOR_WAIT * 1000) < 0)
            goto error;
    while (virTimeBackOffWait(&timebackoff)) {
        ret = connect(monfd, (struct sockaddr *) &addr, sizeof(addr));
        if (ret == 0)
                break;

        if (errno == ENOENT || errno == ECONNREFUSED) {
            /* ENOENT       : Socket may not have shown up yet
                * ECONNREFUSED : Leftover socket hasn't been removed yet */
                continue;
        }
        virReportSystemError(errno, "%s",
                        _("failed to connect to monitor socket"));
        goto error;
    }
    VIR_DEBUG("acrnManagerOpenUnix:connect to monitor sock:fd=%d", monfd);
    return monfd;

 error:
    VIR_FORCE_CLOSE(monfd);
    return -1;
}
void
acrnManagerClose(acrnManagerPtr mon)
{
    if (!mon)
        return;
    virMutexLock(&mon->lock);
    if (mon->fd >= 0) {
        VIR_FORCE_CLOSE(mon->fd);
    }
    virMutexUnlock(&mon->lock);
}

static acrnManagerPtr
acrnManagerOpenInternal(virDomainObjPtr vm,
                        int fd,
                        acrnManagerStopCallback cb)
{
    acrnManagerPtr mon = NULL;

    if (VIR_ALLOC(mon) < 0)
        return NULL;
    if (virMutexInit(&mon->lock) < 0) {
        VIR_FREE(mon);
        return NULL;
    }

    mon->fd = fd;
    mon->vm = virObjectRef(vm);
    mon->stop = cb;

    if (virSetCloseExec(mon->fd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Unable to set monitor close-on-exec flag"));
        goto cleanup;
    }
    if (virSetNonBlock(mon->fd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Unable to put monitor into non-blocking mode"));
        goto cleanup;
    }

    virObjectLock(mon);
    if (!acrnManagerRegister(mon)) {
        virObjectUnlock(mon);
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unable to register monitor events"));
        goto cleanup;
    }
    virObjectUnlock(mon);

    return mon;

cleanup:
    mon->stop = NULL;
    /* The caller owns 'fd' on failure */
    mon->fd = -1;
    acrnManagerClose(mon);
    return NULL;
}

acrnManagerPtr
acrnManagerOpen(virDomainObjPtr vm,
                virDomainChrSourceDefPtr config,
                acrnManagerStopCallback cb)
{
    int fd = -1;
    acrnManagerPtr ret = NULL;

    virObjectRef(vm);

    if (config->type != VIR_DOMAIN_CHR_TYPE_UNIX) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unable to handle monitor type: %s"),
                       virDomainChrTypeToString(config->type));
        goto cleanup;
    }

    fd = acrnManagerOpenUnix(config->data.nix.path);

    if (fd < 0)
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("domain is not running"));
        goto cleanup;
    }

    ret = acrnManagerOpenInternal(vm, fd, cb);

cleanup:
    if (!ret)
        VIR_FORCE_CLOSE(fd);
    virObjectUnref(vm);
    return ret;

}