/** BEGIN COPYRIGHT BLOCK
 * Copyright (C) 2001 Sun Microsystems, Inc. Used by permission.
 * Copyright (C) 2005 Red Hat, Inc.
 * All rights reserved.
 *
 * License: GPL (version 3 or any later version).
 * See LICENSE for details.
 * END COPYRIGHT BLOCK **/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* fileio.c - layer to adjust EOL to use DOS format via PR_Read/Write on NT */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/param.h>
#include <unistd.h>
#include <pwd.h>
#include "slap.h"
#include "pw.h"
#include <prio.h>

PRInt32
slapi_read_buffer(PRFileDesc *fd, void *buf, PRInt32 amount)
{
    PRInt32 rval = 0;

    rval = PR_Read(fd, buf, amount);

    return rval;
}

/*
 * slapi_write_buffer -- same as PR_Write
 *                       except '\r' is added before '\n'.
 * Return value: written bytes not including '\r' characters.
 */
PRInt32
slapi_write_buffer(PRFileDesc *fd, void *buf, PRInt32 amount)
{
    PRInt32 rval = 0;

    rval = PR_Write(fd, buf, amount);

    return rval;
}

/*
 * This function renames a file to a new name.  Unlike PR_Rename or NT rename, this
 * function can be used if the destfilename exists, and it will overwrite the dest
 * file name
 */
int
slapi_destructive_rename(const char *srcfilename, const char *destfilename)
{
    int rv = 0;

    if (rename(srcfilename, destfilename) < 0) {
        rv = errno;
    }

    return rv;
}

/*
 * This function copies the source into the dest, as an independent file.
 *
 * The data is written to "<dest>.tmp" (with the permission bits of the
 * source), fsynced, then renamed over dest, and finally the directory
 * containing dest is fsynced. dest is therefore either the old file or a
 * complete, durable copy of the source; it is never a partial file.
 * Returns 0 on success, or the errno of the failing call.
 */
int
slapi_copy(const char *srcfilename, const char *destfilename)
{
    int rv = 0;
    int srcfd = -1;
    int dstfd = -1;
    char *tmpname = NULL;
    char *dirname = NULL;
    char *slash = NULL;
    char buf[8192];
    struct stat st;

    srcfd = open(srcfilename, O_RDONLY | O_CLOEXEC);
    if (srcfd < 0) {
        return errno;
    }
    if (fstat(srcfd, &st) != 0) {
        rv = errno;
        goto done;
    }

    tmpname = slapi_ch_smprintf("%s.tmp", destfilename);
    dstfd = open(tmpname, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (dstfd < 0) {
        rv = errno;
        goto done;
    }
    /* the umask may have masked some of the bits at creation time */
    if (fchmod(dstfd, st.st_mode & 07777) != 0) {
        rv = errno;
        goto done;
    }

    for (;;) {
        ssize_t nread = read(srcfd, buf, sizeof(buf));
        ssize_t off = 0;

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            rv = errno;
            goto done;
        }
        if (nread == 0) {
            break;
        }
        while (off < nread) {
            ssize_t nwritten = write(dstfd, buf + off, nread - off);
            if (nwritten < 0) {
                if (errno == EINTR) {
                    continue;
                }
                rv = errno;
                goto done;
            }
            off += nwritten;
        }
    }

    if (fsync(dstfd) != 0) {
        rv = errno;
        goto done;
    }
    {
        int fd = dstfd;
        dstfd = -1;
        if (close(fd) != 0) {
            rv = errno;
            goto done;
        }
    }
    if (rename(tmpname, destfilename) != 0) {
        rv = errno;
        goto done;
    }

    /* make the rename durable; work on a copy, the caller's string is const */
    dirname = slapi_ch_strdup(destfilename);
    slash = strrchr(dirname, '/');
    if (slash == NULL) {
        slapi_ch_free_string(&dirname);
        dirname = slapi_ch_strdup(".");
    } else if (slash == dirname) {
        slash[1] = '\0'; /* "/file" -> "/" */
    } else {
        *slash = '\0';
    }
    rv = slapi_fsync_dir(dirname);
    /* the copy is in place; do not remove it even if the dir fsync failed */
    slapi_ch_free_string(&dirname);
    slapi_ch_free_string(&tmpname);
    close(srcfd);
    return rv;

done:
    if (dstfd >= 0) {
        close(dstfd);
    }
    if (rv != 0 && tmpname != NULL) {
        unlink(tmpname);
    }
    slapi_ch_free_string(&tmpname);
    close(srcfd);
    return rv;
}

/*
 * Flush a directory's metadata (e.g. renames, links) to stable storage.
 * Returns 0 on success, or the errno of the failing call. Does not log.
 */
int
slapi_fsync_dir(const char *dirpath)
{
    int rv = 0;
    int fd = open(dirpath, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd < 0) {
        return errno;
    }
    if (fsync(fd) != 0) {
        rv = errno;
    }
    if (close(fd) != 0 && rv == 0) {
        rv = errno;
    }

    return rv;
}
